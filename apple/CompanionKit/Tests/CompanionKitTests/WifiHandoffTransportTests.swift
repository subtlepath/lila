import Foundation
import XCTest
@testable import CompanionKit

private final class HandoffTestClock: @unchecked Sendable {
    private let lock = NSLock()
    private var instant: UInt64 = 0
    func now() -> UInt64 { lock.withLock { instant } }
    func advance(_ seconds: UInt64) { lock.withLock { instant += seconds * 1_000_000_000 } }
}
private enum HandoffFixtureError: Error { case disconnected }
private actor HandoffWireFixture: WifiMessageTransport {
    private let cipher: WifiMessageCipher
    private let transaction: Data
    private let mode: Int
    private let journalPage: Data?
    private let afterRequest: @Sendable () -> Void
    private var count = 0
    private var closed = false
    private var timeout: UInt64 = 0
    init(offer: WifiHandoffOffer, mode: Int = 0, journalPage: Data? = nil, afterRequest: @escaping @Sendable () -> Void = {}) throws {
        cipher = try WifiMessageCipher(key: offer.key, session: offer.session, sending: .readerToApple)
        self.mode = mode; self.afterRequest = afterRequest; transaction = offer.transaction
        self.journalPage = journalPage
    }
    func exchange(_ message: Data, timeoutNanoseconds: UInt64) async throws -> Data {
        count += 1; timeout = timeoutNanoseconds
        let request = try ControlFrame(decoding: await cipher.open(message), authenticated: true)
        if request.command == .journalFormats && request.payload != transaction { throw HandoffFixtureError.disconnected }
        if request.command == .exchangeChanges && (request.payload.count < 16 || request.payload.prefix(16) != transaction) {
            throw HandoffFixtureError.disconnected
        }
        var journalReply = journalPage ?? Data()
        if request.command == .exchangeChanges && request.payload.count == 16 + JournalState.requestSize {
            guard Data(request.payload.dropFirst(16)).prefix(4) == Data([0x4a, 0x53, 0x54, 1]) else {
                throw HandoffFixtureError.disconnected
            }
            journalReply = Data([0x4a, 0x53, 0x53, 1])
            journalReply.appendLittleEndian(0, count: 4)
            journalReply.appendLittleEndian(512, count: 2)
            journalReply.append(contentsOf: [0, 0])
            journalReply.append(try TintaJournalFrontier.digest([]))
        } else if request.command == .exchangeChanges && request.payload.count == 16 + JournalMergeReadiness.requestSize {
            journalReply = Data([0x4a, 0x52, 0x52, 1, 0, 0, 0, 0])
        } else if request.command == .exchangeChanges && request.payload.count == 80 {
            let backup = try LegacyBackupRequest(decoding: Data(request.payload.dropFirst(16)))
            guard backup.transaction == transaction else { throw HandoffFixtureError.disconnected }
            journalReply = Data([0x54, 0x4c, 0x53, 1, 0, 0xff, 1, 0]) + transaction + Data(count: 8)
        } else if request.command == .exchangeChanges && request.payload.count == 16 + TintaMigrationAdmission.byteCount {
            let admission = try TintaMigrationAdmission(decoding: Data(request.payload.dropFirst(16)))
            guard admission.merge.transaction == transaction else { throw HandoffFixtureError.disconnected }
            journalReply = Data([1, 0, 0, 0]) + transaction
            journalReply.appendLittleEndian(UInt64(admission.merge.previous.count), count: 4)
        } else if request.command == .exchangeChanges && request.payload.count != 56 {
            let merge = try JournalMergeRequest.decodePayload(Data(request.payload.dropFirst(16)))
            guard merge.transaction == transaction else { throw HandoffFixtureError.disconnected }
            journalReply = Data([1, 0, 0, 0]) + transaction
            journalReply.appendLittleEndian(0, count: 4)
        }
        if request.command == .readContent {
            guard request.payload.prefix(16) == transaction else { throw HandoffFixtureError.disconnected }
            let read = try ReaderContentReadRequest(decoding: Data(request.payload.dropFirst(16)))
            let count = Int(min(UInt64(read.maximumBytes), read.manifest.length - read.offset))
            journalReply = Data([0x4c, 0x43, 0x53, 1, 0])
            journalReply.append(read.generation); journalReply.append(read.manifest.content.digest)
            journalReply.appendLittleEndian(read.offset, count: 8)
            journalReply.appendLittleEndian(UInt64(count), count: 2)
            journalReply.append(Data(repeating: 7, count: count))
            if mode == 6 { journalReply[5] ^= 1 }
        }
        afterRequest()
        if mode == 3 { throw HandoffFixtureError.disconnected }
        let reply = try ControlFrame(command: mode == 2 ? .commit : request.command,
            response: true, requestID: mode == 1 ? request.requestID + 1 : request.requestID,
            payload: request.command == .journalFormats ? Data([0, 6]) :
                     request.command == .exchangeChanges || request.command == .readContent ? journalReply :
                     request.command == .wifiHandoff && mode != 5 ? Data([0]) : Data())
        var encrypted = try await cipher.seal(reply.encoded())
        if mode == 4 { encrypted[31] ^= 1 }
        return encrypted
    }
    func close() async { closed = true; await cipher.invalidate() }
    func statistics() -> (Int, Bool, UInt64) { (count, closed, timeout) }
}

final class WifiHandoffTransportTests: XCTestCase {
    func testContentReadsUseEncryptedTransactionAndGenerationBindings() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let wire = try HandoffWireFixture(offer: offer)
        let transport = try transport(offer, wire, clock)
        let manifest = try ContentManifest(content: ContentID(String(repeating: "12", count: 32)), kind: .epub,
            length: 1000, formatVersion: 1, logicalIdentity: Data(count: 16))
        let request = try ReaderContentReadRequest(generation: offer.storageGeneration, manifest: manifest,
            offset: 961, maximumBytes: 961)
        let reply = try await transport.readContent(request, requestID: 19)
        XCTAssertEqual(reply.result, .ok); XCTAssertEqual(reply.bytes, Data(repeating: 7, count: 39))
        let foreign = try ControlFrame(command: .readContent, requestID: 20,
            payload: Data(repeating: 9, count: 16) + request.encoded)
        do { _ = try await transport.exchange(foreign); XCTFail("Foreign transaction accepted") }
        catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        let stats = await wire.statistics(); XCTAssertEqual(stats.0, 1); XCTAssertTrue(stats.1)
    }
    func testForeignCardContentReadFailsBeforeEncryptedHTTP() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let wire = try HandoffWireFixture(offer: offer)
        let transport = try transport(offer, wire, clock)
        let manifest = try ContentManifest(content: ContentID(String(repeating: "12", count: 32)), kind: .epub,
            length: 5, formatVersion: 1, logicalIdentity: Data(count: 16))
        let request = try ReaderContentReadRequest(generation: Data(repeating: 9, count: 16), manifest: manifest,
            offset: 0, maximumBytes: 3)
        do { _ = try await transport.readContent(request, requestID: 20); XCTFail("Foreign card accepted") }
        catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        let stats = await wire.statistics(); XCTAssertEqual(stats.0, 0); XCTAssertTrue(stats.1)
    }
    func testWrongContentReplyBindingClosesEncryptedTransport() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let wire = try HandoffWireFixture(offer: offer, mode: 6)
        let transport = try transport(offer, wire, clock)
        let manifest = try ContentManifest(content: ContentID(String(repeating: "12", count: 32)), kind: .epub,
            length: 5, formatVersion: 1, logicalIdentity: Data(count: 16))
        let request = try ReaderContentReadRequest(generation: offer.storageGeneration, manifest: manifest,
            offset: 0, maximumBytes: 3)
        do { _ = try await transport.readContent(request, requestID: 21); XCTFail("Foreign reply accepted") }
        catch { XCTAssertEqual(error as? ProtocolError, .value) }
        let stats = await wire.statistics(); XCTAssertEqual(stats.0, 1); XCTAssertTrue(stats.1)
    }
    func testCollectorImportsEmptyJournalOverEncryptedHandoff() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        var page = Data([1, 1, 0, 0])
        page.append(Data(count: 8))
        page.append(try TintaJournalFrontier.digest([]))
        page.append(Data(count: 4))
        let wire = try HandoffWireFixture(offer: offer, journalPage: page)
        let transport = try transport(offer, wire, clock)
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let receipt = try await JournalExportCollector().collect(wifi: transport, library: library, maximumEvents: 0)
        XCTAssertEqual(receipt.count, 0); XCTAssertEqual(receipt.inserted, 0)
        XCTAssertEqual(receipt.frontier, try TintaJournalFrontier.digest([]))
        let stats = await wire.statistics(); XCTAssertEqual(stats.0, 1); XCTAssertFalse(stats.1)
    }
    func testJournalExportUsesEncryptedTransactionBinding() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let fixture = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/JournalExportPage-v1.fixture"))
        let wire = try HandoffWireFixture(offer: offer, journalPage: fixture)
        let transport = try transport(offer, wire, clock)
        let page = try await transport.journalExportPage(requestID: 17)
        XCTAssertEqual(page.cursor.next, 1)
        XCTAssertEqual(page.mutation?.event.kind, .star)
        let foreign = try ControlFrame(command: .exchangeChanges, requestID: 18,
                                       payload: Data(repeating: 9, count: 16) + JournalExportCursor.start.payload)
        do { _ = try await transport.exchange(foreign); XCTFail("Foreign journal request accepted") }
        catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        let stats = await wire.statistics()
        XCTAssertEqual(stats.0, 1); XCTAssertTrue(stats.1)
    }
    func testJournalMergeUsesEncryptedTransactionAndDeclarationBindings() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let wire = try HandoffWireFixture(offer: offer)
        let transport = try transport(offer, wire, clock)
        let snapshot = try JournalMergeSnapshot(count: 0, recordSize: 1024, frontier: Data(repeating: 7, count: 32))
        let declaration = try JournalMergeDeclaration(generation: offer.storageGeneration, transaction: offer.transaction,
            owner: offer.installation, previous: snapshot, merged: snapshot)
        let reply = try await transport.exchangeJournalMerge(.begin(declaration), requestID: 17)
        XCTAssertEqual(reply.result, .ok)
        XCTAssertEqual(reply.count, 0)
        let foreign = try JournalMergeDeclaration(generation: offer.storageGeneration, transaction: Data(repeating: 9, count: 16),
            owner: offer.installation, previous: snapshot, merged: snapshot)
        do { _ = try await transport.exchangeJournalMerge(.begin(foreign), requestID: 18); XCTFail("Foreign merge accepted") }
        catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        let stats = await wire.statistics()
        XCTAssertEqual(stats.0, 1); XCTAssertTrue(stats.1)
    }
    func testReadinessQueryUsesEncryptedGenerationAndTransactionBindings() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let wire = try HandoffWireFixture(offer: offer), transport = try transport(offer, wire, clock)
        let snapshot = try JournalMergeSnapshot(count: 0, recordSize: 512, frontier: TintaJournalFrontier.digest([]))
        let result = try await transport.journalMergeReadiness(snapshot: snapshot, requestID: 17)
        XCTAssertEqual(result, .ready)
        let foreign = try JournalMergeReadiness.request(generation: Data(repeating: 99, count: 16), snapshot: snapshot, requestID: 18)
        let bound = try ControlFrame(command: .exchangeChanges, requestID: 18, payload: offer.transaction + foreign.payload)
        do {
            _ = try await transport.exchange(bound)
            XCTFail("foreign generation must not reach Wi-Fi")
        } catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        let stats = await wire.statistics()
        XCTAssertEqual(stats.0, 1)
    }
    func testOrdinaryMergeControlRejectsForeignLeaseBindingsBeforeSending() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let wire = try HandoffWireFixture(offer: offer), transport = try transport(offer, wire, clock)
        let snapshot = try JournalMergeSnapshot(count: 0, recordSize: 1024, frontier: Data(repeating: 7, count: 32))
        let declaration = try JournalMergeDeclaration(generation: offer.storageGeneration, transaction: offer.transaction,
            owner: offer.installation, previous: snapshot, merged: snapshot)
        let request = try JournalMergeRequest.begin(declaration).frame(requestID: 17)
        let reply = try JournalMergeReply.decode(await transport.exchangeJournalMergeControl(request), request: request)
        XCTAssertEqual(reply.result, .ok)
        for field in 0..<3 {
            let foreign = try JournalMergeDeclaration(generation: field == 0 ? Data(repeating: 99, count: 16) : offer.storageGeneration,
                transaction: field == 1 ? Data(repeating: 99, count: 16) : offer.transaction,
                owner: field == 2 ? Data(repeating: 99, count: 16) : offer.installation, previous: snapshot, merged: snapshot)
            do {
                _ = try await transport.exchangeJournalMergeControl(JournalMergeRequest.begin(foreign).frame(requestID: UInt32(18 + field)))
                XCTFail("foreign lease binding must be refused")
            } catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        }
        let stats = await wire.statistics()
        XCTAssertEqual(stats.0, 1)
    }
    func testMigrationAdmissionUsesEncryptedReaderOwnerCardAndTransactionBindings() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let wire = try HandoffWireFixture(offer: offer), transport = try transport(offer, wire, clock)
        let previous = try JournalMergeSnapshot(count: 0, recordSize: 512, frontier: Data(repeating: 7, count: 32))
        let merged = try JournalMergeSnapshot(count: 1, recordSize: 1024, frontier: Data(repeating: 8, count: 32))
        let merge = try JournalMergeDeclaration(generation: offer.storageGeneration, transaction: offer.transaction,
            owner: offer.installation, previous: previous, merged: merged)
        let admission = try TintaMigrationAdmission(merge: merge, course: Data(repeating: 4, count: 16),
            resource: Data(repeating: 5, count: 32), backupTransaction: Data(repeating: 6, count: 16),
            reader: offer.reader, backupManifest: Data(repeating: 7, count: 32))
        let reply = try await transport.admitTintaMigration(admission, requestID: 17)
        XCTAssertEqual(reply.result, .ok)
        XCTAssertEqual(reply.count, 0)
        let foreign = try TintaMigrationAdmission(merge: merge, course: admission.course, resource: admission.resource,
            backupTransaction: admission.backupTransaction, reader: Data(repeating: 99, count: 16),
            backupManifest: admission.backupManifest)
        do { _ = try await transport.admitTintaMigration(foreign, requestID: 18); XCTFail("Foreign reader admitted") }
        catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        let stats = await wire.statistics()
        XCTAssertEqual(stats.0, 1)
        let bound = try ControlFrame(command: .exchangeChanges, requestID: 19, payload: offer.transaction + foreign.bytes)
        do { _ = try await transport.exchange(bound); XCTFail("Foreign bound admission accepted") }
        catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        let rejected = await wire.statistics()
        XCTAssertEqual(rejected.0, 1); XCTAssertTrue(rejected.1)
    }
    func testActiveJournalSnapshotUsesEncryptedHandoffAndRejectsForeignCard() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let wire = try HandoffWireFixture(offer: offer), transport = try transport(offer, wire, clock)
        let snapshot = try await transport.journalState(requestID: 17)
        XCTAssertEqual(snapshot.count, 0); XCTAssertEqual(snapshot.recordSize, 512)
        XCTAssertEqual(snapshot.frontier, try TintaJournalFrontier.digest([]))
        let foreign = try JournalState.request(generation: Data(repeating: 99, count: 16), requestID: 18)
        let bound = try ControlFrame(command: .exchangeChanges, requestID: 18, payload: offer.transaction + foreign.payload)
        do { _ = try await transport.exchange(bound); XCTFail("Foreign card state queried") }
        catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        let stats = await wire.statistics(); XCTAssertEqual(stats.0, 1); XCTAssertTrue(stats.1)
    }
    func testJournalFormatsQueryUsesEncryptedTransactionBinding() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let wire = try HandoffWireFixture(offer: offer)
        let transport = try transport(offer, wire, clock)
        let formats = try await transport.journalHeaderVersions(requestID: 17)
        XCTAssertEqual(formats, [2, 3])
        let stats = await wire.statistics()
        XCTAssertEqual(stats.0, 1)
        XCTAssertFalse(stats.1)
        let foreign = try ControlFrame(command: .journalFormats, requestID: 18, payload: Data(repeating: 9, count: 16))
        do { _ = try await transport.exchange(foreign); XCTFail("foreign metadata request") }
        catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        let rejected = await wire.statistics()
        XCTAssertEqual(rejected.0, 1)
        XCTAssertTrue(rejected.1)
    }
    func testForeignFinishBindingClosesWithoutNetworkTraffic() async throws {
        for foreignTransaction in [false, true] {
            let offer = try offer(), clock = HandoffTestClock()
            let wire = try HandoffWireFixture(offer: offer)
            let transport = try transport(offer, wire, clock)
            let request = try WifiHandoffCommands.cancel(
                transaction: foreignTransaction ? Data(repeating: 9, count: 16) : offer.transaction,
                session: foreignTransaction ? offer.session : Data(repeating: 9, count: 16), requestID: 19)
            do { _ = try await transport.exchange(request); XCTFail("foreign finish") }
            catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
            let stats = await wire.statistics()
            XCTAssertEqual(stats.0, 0)
            XCTAssertTrue(stats.1)
        }
    }
    func testFinishAcknowledgementAndFailuresAlwaysCloseSession() async throws {
        for mode in [0, 3, 5] {
            let offer = try offer(), clock = HandoffTestClock()
            let wire = try HandoffWireFixture(offer: offer, mode: mode)
            let transport = try transport(offer, wire, clock)
            do {
                try await transport.finish(requestID: 19)
                XCTAssertEqual(mode, 0)
            } catch { XCTAssertNotEqual(mode, 0) }
            let stats = await wire.statistics()
            XCTAssertEqual(stats.0, 1)
            XCTAssertTrue(stats.1)
            do { try await transport.finish(requestID: 20); XCTFail("finished session cannot be reused") }
            catch { XCTAssertEqual(error as? WifiHandoffTransportError, .invalidated) }
        }
    }
    private func offer() throws -> WifiHandoffOffer {
        var bytes = Data([1])
        for value: UInt8 in 1...5 { bytes.append(Data(repeating: value, count: 16)) }
        bytes.append(Data(0..<32)); bytes.append(contentsOf: [192, 168, 4, 1])
        bytes.appendLittleEndian(8080, count: 2); bytes.appendLittleEndian(30, count: 2)
        return try WifiHandoffOffer(decoding: bytes)
    }
    private func transport(_ offer: WifiHandoffOffer, _ wire: HandoffWireFixture, _ clock: HandoffTestClock) throws -> WifiHandoffTransport {
        try WifiHandoffTransport(offer: offer, reader: offer.reader, storageGeneration: offer.storageGeneration,
            installation: offer.installation, transaction: offer.transaction, receivedAt: 0, wire: wire, now: { clock.now() })
    }
    func testEncryptedTransferFramesAndIdleRenewal() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let wire = try HandoffWireFixture(offer: offer)
        let transport = try transport(offer, wire, clock)
        let request = try TransferCommands.transaction(.abort, identity: offer.transaction, requestID: 1)
        clock.advance(29)
        let first = try await transport.exchange(request)
        XCTAssertEqual(first.command, .abort)
        XCTAssertEqual(first.requestID, 1)
        clock.advance(29)
        let second = try await transport.exchange(request)
        XCTAssertEqual(second.requestID, 1)
        let stats = await wire.statistics()
        XCTAssertEqual(stats.0, 2)
        XCTAssertFalse(stats.1)
        XCTAssertEqual(stats.2, 1_000_000_000)
        clock.advance(30)
        do { _ = try await transport.exchange(request); XCTFail("idle expiry") }
        catch { XCTAssertEqual(error as? WifiHandoffTransportError, .expired) }
        let ended = await wire.statistics()
        XCTAssertEqual(ended.0, 2)
        XCTAssertTrue(ended.1)
    }
    func testFailuresCloseSessionAndNeverReuseCipher() async throws {
        for mode in 1...4 {
            let offer = try offer(), clock = HandoffTestClock()
            let wire = try HandoffWireFixture(offer: offer, mode: mode)
            let transport = try transport(offer, wire, clock)
            let request = try TransferCommands.transaction(.abort, identity: offer.transaction, requestID: 1)
            do { _ = try await transport.exchange(request); XCTFail("bad or lost response") } catch {}
            let failed = await wire.statistics()
            XCTAssertTrue(failed.1)
            do { _ = try await transport.exchange(request); XCTFail("closed handoff") }
            catch { XCTAssertEqual(error as? WifiHandoffTransportError, .invalidated) }
            let final = await wire.statistics()
            XCTAssertEqual(final.0, 1)
        }
    }
    func testBindingAndOfferExpiryPreventNetworkTraffic() async throws {
        let offer = try offer()
        for expired in [false, true] {
            let clock = HandoffTestClock(), wire = try HandoffWireFixture(offer: offer)
            let transport = try transport(offer, wire, clock)
            if expired { clock.advance(30) }
            let request = try TransferCommands.transaction(.abort,
                identity: expired ? offer.transaction : Data(repeating: 9, count: 16), requestID: 1)
            do { _ = try await transport.exchange(request); XCTFail("invalid handoff") }
            catch { XCTAssertEqual(error as? WifiHandoffTransportError, expired ? .expired : .binding) }
            let stats = await wire.statistics()
            XCTAssertEqual(stats.0, 0)
            XCTAssertTrue(stats.1)
        }
        let clock = HandoffTestClock(), wire = try HandoffWireFixture(offer: offer, afterRequest: { })
        XCTAssertThrowsError(try WifiHandoffTransport(offer: offer, reader: Data(repeating: 9, count: 16),
            storageGeneration: offer.storageGeneration, installation: offer.installation,
            transaction: offer.transaction, receivedAt: 0, wire: wire, now: { clock.now() }))
    }
    func testResponseAfterExpiryCannotExtendSession() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let wire = try HandoffWireFixture(offer: offer, afterRequest: { clock.advance(30) })
        let transport = try transport(offer, wire, clock)
        let request = try TransferCommands.transaction(.abort, identity: offer.transaction, requestID: 1)
        do { _ = try await transport.exchange(request); XCTFail("late response") }
        catch { XCTAssertEqual(error as? WifiHandoffTransportError, .expired) }
        let stats = await wire.statistics()
        XCTAssertTrue(stats.1)
    }
    func testBeginFramesEnforceInstallationStorageAndTransactionBinding() async throws {
        for declared in [false, true] {
            for mismatch in 0...3 {
                let offer = try offer(), clock = HandoffTestClock()
                let wire = try HandoffWireFixture(offer: offer)
                let transport = try transport(offer, wire, clock)
                let different = Data(repeating: 9, count: 16)
                let hash = Data(repeating: 6, count: 32)
                let state = try TransferState(transaction: mismatch == 1 ? different : offer.transaction,
                    owner: mismatch == 2 ? different : offer.installation,
                    storageGeneration: mismatch == 3 ? different : offer.storageGeneration,
                    contentHash: hash, length: 10)
                let request: ControlFrame
                if declared {
                    let manifest = try ContentManifest(content: ContentID(String(repeating: "06", count: 32)),
                        kind: .course, length: 10, formatVersion: 1, logicalIdentity: Data(repeating: 7, count: 16))
                    request = try TransferCommands.begin(TransferDeclaration(manifest: manifest, state: state), requestID: 1)
                } else { request = try TransferCommands.begin(state, requestID: 1) }
                if mismatch == 0 {
                    let response = try await transport.exchange(request)
                    XCTAssertEqual(response.command, .beginTransfer)
                } else {
                    do { _ = try await transport.exchange(request); XCTFail("foreign begin") }
                    catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
                }
                let stats = await wire.statistics()
                XCTAssertEqual(stats.0, mismatch == 0 ? 1 : 0)
            }
        }
    }
}


extension WifiHandoffTransportTests {
    func testBackupUsesEncryptedTransactionAndCardBindings() async throws {
        let offer = try offer(), clock = HandoffTestClock()
        let wire = try HandoffWireFixture(offer: offer)
        let transport = try transport(offer, wire, clock)
        let request = try LegacyBackupRequest(operation: .close, bound: true,
            course: Data(repeating: 7, count: 16), transaction: offer.transaction, generation: offer.storageGeneration)
        let reply = try await transport.exchangeLegacyBackup(request, requestID: 17)
        XCTAssertEqual(reply.result, .ok)
        let foreign = try LegacyBackupRequest(operation: .close, bound: true,
            course: request.course, transaction: offer.transaction, generation: Data(repeating: 9, count: 16))
        do { _ = try await transport.exchangeLegacyBackup(foreign, requestID: 18); XCTFail("Foreign card accepted") }
        catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        do {
            _ = try await LegacyBackupCollector().collect(wifi: transport, course: request.course,
                transaction: Data(repeating: 9, count: 16), bound: true, capture: false,
                staging: root.appendingPathComponent("staging"), library: library, vault: vault)
            XCTFail("Collector accepted foreign handoff transaction")
        } catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        let stats = await wire.statistics()
        XCTAssertEqual(stats.0, 1); XCTAssertFalse(stats.1)
    }
}

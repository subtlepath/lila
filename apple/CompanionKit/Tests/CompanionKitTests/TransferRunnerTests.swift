import Foundation
import XCTest
import CSQLite
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

private enum RunnerFixtureError: Error { case disconnected }
private actor FirmwareInstallFixture: CompanionTransport {
    var requests: [Data] = []
    private var lostReply = true
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        guard request.command == .installFirmware else { throw ProtocolError.value }
        let install = try FirmwareInstallRequest(decoding: request.payload)
        requests.append(install.bytes)
        if lostReply { lostReply = false; throw RunnerFixtureError.disconnected }
        return try ControlFrame(command: .installFirmware, response: true, requestID: request.requestID,
            payload: Data([0]) + install.bytes.subdata(in: 20..<36))
    }
    func count() -> Int { requests.count }
}
private actor ReaderTransferFixture: CompanionTransport {
    private var state: TransferState?
    private let beginPhase: TransferPhase?
    private var lostChunk: Bool
    private var lostAbort: Bool
    private var lostCommit: Bool
    private var badOffset: Bool
    private var installingCommit: Bool
    private var rejectCommit: Bool
    private var lostStatus: Bool
    private let rejectionStatus: TransferPhase?
    private let foreignStatus: Bool
    private var cancelCommit: Bool
    private var cancelChunk: Bool
    private var cancelInstallingBegin: Bool
    private var received = Data()
    private var declarations: [TransferDeclaration] = []
    private var destinations: [String] = []
    private var consents: [CourseSwitchRequest] = []
    func savedConsents() -> [CourseSwitchRequest] { consents }
    func declaredBegins() -> [TransferDeclaration] { declarations }
    func declaredDestinations() -> [String] { destinations }
    func restoreStatusConnection() { lostStatus = false }
    private var exchanges = 0
    private var commitRequests = 0
    private let deselectAtOffset: (LibraryStore, Data, ContentID, UInt64)?
    private var deselected = false
    init(beginPhase: TransferPhase? = nil, lostChunk: Bool = false, lostCommit: Bool = false, lostAbort: Bool = false, badOffset: Bool = false,
         installingCommit: Bool = false, rejectCommit: Bool = false, lostStatus: Bool = false,
         rejectionStatus: TransferPhase? = nil, foreignStatus: Bool = false,
         cancelInstallingBegin: Bool = false, cancelChunk: Bool = false, cancelCommit: Bool = false,
         deselectAtOffset: (LibraryStore, Data, ContentID, UInt64)? = nil) {
        self.beginPhase = beginPhase
        self.cancelCommit = cancelCommit
        self.cancelChunk = cancelChunk
        self.deselectAtOffset = deselectAtOffset
        self.lostAbort = lostAbort
        self.lostChunk = lostChunk; self.lostCommit = lostCommit; self.badOffset = badOffset
        self.installingCommit = installingCommit; self.cancelInstallingBegin = cancelInstallingBegin
        self.rejectCommit = rejectCommit
        self.lostStatus = lostStatus
        self.rejectionStatus = rejectionStatus; self.foreignStatus = foreignStatus
        received.reserveCapacity(5000)
    }
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        exchanges += 1
        switch request.command {
        case .exchangeChanges:
            let consent = try CourseSwitchRequest(decoding: request.payload)
            guard let current = state, current.phase == .receiving,
                  consent.transaction == current.transaction, consent.generation == current.storageGeneration,
                  consent.nextHash == current.contentHash else { throw RunnerFixtureError.disconnected }
            consents.append(consent)
            return try ControlFrame(command: .exchangeChanges, response: true, requestID: request.requestID,
                payload: Data([0]) + consent.transaction)
        case .beginTransfer:
            let initial: TransferState
            if request.payload.count >= 163,
               let declaration = try? TransferDeclaration(decoding: Data(request.payload.prefix(162))) {
                declarations.append(declaration)
                let path = declaration.manifest.kind == .font ? Data(request.payload.dropFirst(163)) :
                    declaration.manifest.kind == .dictionary ? Data(("/dictionaries/" + declaration.manifest.content.hex + "/dictionary").utf8) :
                    Data((declaration.manifest.kind == .firmware ? "/Companion/firmware.bin" : "/tinta/course.pack").utf8)
                if declaration.manifest.kind == .dictionary { destinations.append(String(decoding: path, as: UTF8.self)) }
                if declaration.manifest.kind == .font {
                    guard let destination = String(data: path, encoding: .utf8), destination.hasPrefix("/fonts/") else {
                        throw ProtocolError.value
                    }
                    destinations.append(destination)
                }
                guard request.payload[162] == UInt8(path.count), Data(request.payload.dropFirst(163)) == path else {
                    throw RunnerFixtureError.disconnected
                }
                initial = declaration.state
            } else {
                initial = try TransferState(decoding: Data(request.payload.prefix(99)))
            }
            if state == nil {
                if let beginPhase {
                    state = try TransferState(transaction: initial.transaction, owner: initial.owner,
                        storageGeneration: initial.storageGeneration, contentHash: initial.contentHash,
                        length: initial.length, durableOffset: initial.length, phase: beginPhase)
                } else { state = initial }
            }
            if state?.phase == .installing, cancelInstallingBegin {
                cancelInstallingBegin = false
                withUnsafeCurrentTask { $0?.cancel() }
            }
        case .transferChunk:
            guard let current = state else { throw RunnerFixtureError.disconnected }
            var reader = ByteReader(request.payload)
            _ = try reader.take(16)
            let offset = try reader.number(8)
            let bytes = try reader.take(request.payload.count - 24)
            guard offset == current.durableOffset else { throw RunnerFixtureError.disconnected }
            received.append(bytes)
            state = try TransferState(transaction: current.transaction, owner: current.owner, storageGeneration: current.storageGeneration,
                                      contentHash: current.contentHash, length: current.length, durableOffset: offset + UInt64(bytes.count))
            if let (library, reader, content, threshold) = deselectAtOffset,
               !deselected, offset + UInt64(bytes.count) >= threshold {
                deselected = true
                _ = try await library.setReaderSelection(reader: reader, content: content, selected: false)
            }
            if cancelChunk {
                cancelChunk = false
                withUnsafeCurrentTask { $0?.cancel() }
            }
            if lostChunk { lostChunk = false; throw RunnerFixtureError.disconnected }
            if badOffset {
                badOffset = false
                return try reply(request, state: current)
            }
        case .abort:
            guard let current = state else {
                return try ControlFrame(command: .abort, response: true, requestID: request.requestID, payload: Data([1]))
            }
            state = try TransferState(transaction: current.transaction, owner: current.owner, storageGeneration: current.storageGeneration,
                contentHash: current.contentHash, length: current.length, durableOffset: current.durableOffset, phase: .aborted)
            if lostAbort { lostAbort = false; throw RunnerFixtureError.disconnected }
        case .commit:
            commitRequests += 1
            if rejectCommit {
                return try ControlFrame(command: .commit, response: true, requestID: request.requestID, payload: Data([2]))
            }
            guard let current = state else { throw RunnerFixtureError.disconnected }
            state = try TransferState(transaction: current.transaction, owner: current.owner, storageGeneration: current.storageGeneration,
                                      contentHash: current.contentHash, length: current.length, durableOffset: current.length,
                                      phase: installingCommit ? .installing : .committed)
            installingCommit = false
            if cancelCommit {
                cancelCommit = false
                withUnsafeCurrentTask { $0?.cancel() }
                throw CancellationError()
            }
            if lostCommit { lostCommit = false; throw RunnerFixtureError.disconnected }
        case .transferStatus:
            if lostStatus { throw RunnerFixtureError.disconnected }
            if let current = state, let rejectionStatus {
                let reported = try TransferState(transaction: current.transaction,
                    owner: foreignStatus ? Data(repeating: 9, count: 16) : current.owner,
                    storageGeneration: current.storageGeneration, contentHash: current.contentHash,
                    length: current.length, durableOffset: current.durableOffset, phase: rejectionStatus)
                return try reply(request, state: reported)
            }
        default: throw RunnerFixtureError.disconnected
        }
        guard let state else { throw RunnerFixtureError.disconnected }
        return try reply(request, state: state)
    }
    private func reply(_ request: ControlFrame, state: TransferState) throws -> ControlFrame {
        try ControlFrame(command: request.command, response: true, requestID: request.requestID, payload: Data([0]) + state.encoded())
    }
    func bytes() -> Data { received }
    func count() -> Int { exchanges }
    func commits() -> Int { commitRequests }
}

private actor EncryptedReaderTransferFixture: WifiMessageTransport {
    private let reader: ReaderTransferFixture
    private let cipher: WifiMessageCipher
    private var closed = false
    init(reader: ReaderTransferFixture, offer: WifiHandoffOffer) throws {
        self.reader = reader
        cipher = try WifiMessageCipher(key: offer.key, session: offer.session, sending: .readerToApple)
    }
    func exchange(_ message: Data, timeoutNanoseconds: UInt64) async throws -> Data {
        guard !closed else { throw RunnerFixtureError.disconnected }
        let request = try ControlFrame(decoding: await cipher.open(message), authenticated: true)
        let reply = try await reader.exchange(request)
        return try await cipher.seal(reply.encoded())
    }
    func close() async { closed = true; await cipher.invalidate() }
    func isClosed() -> Bool { closed }
}

final class TransferRunnerTests: XCTestCase, @unchecked Sendable {
    private func device(generation: UInt8 = 3, capabilities: UInt32 = 0) throws -> DeviceDescriptor {
        var bytes = Data([1, 1])
        bytes.append(Data(repeating: 2, count: 16)); bytes.append(Data(repeating: generation, count: 16))
        bytes.append(1); bytes.appendLittleEndian(UInt64(capabilities), count: 4); bytes.append(contentsOf: [80, 1, 1])
        bytes.append(Data(repeating: 0, count: 32))
        return try DeviceDescriptor(decoding: bytes)
    }
    private func setup(_ root: URL, count: Int = 4103) async throws -> (LibraryStore, ContentVault, TransferJob, Data) {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let bytes = Data((0 ..< count).map { UInt8(truncatingIfNeeded: $0) })
        let source = root.appendingPathComponent("book.epub")
        try bytes.write(to: source)
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let object = try await vault.importFile(source)
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        try await library.put(LibraryContent(id: object.id, kind: .epub, length: object.length, title: "Book", originalFilename: "book.epub"))
        let job = try await library.enqueue(content: object.id, reader: Data(repeating: 2, count: 16),
                                            storageGeneration: Data(repeating: 3, count: 16), installation: Data(repeating: 4, count: 16))
        return (library, vault, job, bytes)
    }
    private func setupCourse(_ root: URL, confirm: Bool = true) async throws -> (LibraryStore, ContentVault, TransferJob, Data) {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { workspace.deleteLastPathComponent() }
        let source = workspace.appendingPathComponent("test/tinta/fixtures/mini.pack")
        let metadata = try CoursePackInspector.inspect(source)
        let bytes = try Data(contentsOf: source)
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let object = try await vault.importFile(source)
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        try await library.putCoursePack(LibraryContent(id: object.id, kind: .course, length: object.length,
            title: "Course", originalFilename: "course.pack", languages: [metadata.locale]), metadata: metadata)
        if confirm { _ = try await library.associateCourse(object.id, confirmedIdentity: Data(repeating: 7, count: 16)) }
        let job = try await library.enqueue(content: object.id, reader: Data(repeating: 2, count: 16),
            storageGeneration: Data(repeating: 3, count: 16), installation: Data(repeating: 4, count: 16))
        return (library, vault, job, bytes)
    }
    private func setupDictionary(_ root: URL, compressed: Bool = false) async throws -> (LibraryStore, ContentVault, TransferJob) {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let source = root.appendingPathComponent("Dictionary.zip")
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { workspace.deleteLastPathComponent() }
        let fixture = workspace.appendingPathComponent("protocol/fixtures/" + (compressed ? "DictionaryBundle-dictzip.fixture" : "DictionaryBundle-plain.fixture"))
        try Data(contentsOf: fixture).write(to: source)
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let content = try await ContentImporter(vault: vault, library: library).importDictionaryBundle(source).content
        let job = try await library.enqueue(content: content.id, reader: Data(repeating: 2, count: 16),
            storageGeneration: Data(repeating: 3, count: 16), installation: Data(repeating: 4, count: 16))
        return (library, vault, job)
    }
    func testDictionaryRequiresExplicitCapabilitiesBeforeRetainingOrSendingDeclaration() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job) = try await setupDictionary(root)
        let reader = ReaderTransferFixture()
        let runner = TransferRunner(library: library, vault: vault)
        for capabilities: UInt32 in [0, 1, 1 << 7, UInt32.max & ~UInt32(1 << 7), UInt32.max & ~UInt32(1)] {
            do {
                _ = try await runner.prepareDeclaration(job.id, device: device(capabilities: capabilities), installation: job.installation)
                XCTFail("Dictionary requires both capability bits")
            } catch { XCTAssertEqual(error as? TransferRunnerError, .unsupportedContent) }
            do {
                _ = try await runner.run(job.id, device: device(capabilities: capabilities), transport: reader)
                XCTFail("Unsupported reader must not receive dictionary data")
            } catch { XCTAssertEqual(error as? TransferRunnerError, .unsupportedContent) }
        }
        let sent = await reader.declaredBegins()
        let retained = try await library.retainedTransferDeclaration(job.id)
        XCTAssertTrue(sent.isEmpty)
        XCTAssertNil(retained)
    }
    func testDictionaryRevalidatesSemanticsBeforeSendingOrRetainingDeclaration() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let source = root.appendingPathComponent("invalid.zip")
        try Data(repeating: 0, count: 100).write(to: source)
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let object = try await vault.importFile(source)
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        try await library.put(LibraryContent(id: object.id, kind: .dictionary, length: object.length,
            title: "Invalid", originalFilename: "invalid.zip"))
        let job = try await library.enqueue(content: object.id, reader: Data(repeating: 2, count: 16),
            storageGeneration: Data(repeating: 3, count: 16), installation: Data(repeating: 4, count: 16))
        let runner = TransferRunner(library: library, vault: vault)
        let reader = ReaderTransferFixture()
        do {
            _ = try await runner.prepareDeclaration(job.id, device: device(capabilities: 1 | (1 << 7)), installation: job.installation)
            XCTFail("Malformed ZIP must not become a retained declaration")
        } catch {}
        do {
            _ = try await runner.run(job.id, device: device(capabilities: 1 | (1 << 7)), transport: reader)
            XCTFail("Malformed ZIP must not reach the reader")
        } catch {}
        let sent = await reader.declaredBegins()
        let retained = try await library.retainedTransferDeclaration(job.id)
        XCTAssertTrue(sent.isEmpty)
        XCTAssertNil(retained)
    }
    func testDictionaryResumesAfterLostReplyAndRenameAcrossDatabaseRestart() async throws {
        for compressed in [false, true] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let (library, vault, job) = try await setupDictionary(root, compressed: compressed)
            let reader = ReaderTransferFixture(lostChunk: true, lostCommit: true)
            let capabilities: UInt32 = 1 | (1 << 7)
            let runner = TransferRunner(library: library, vault: vault)
            do { _ = try await runner.run(job.id, device: device(capabilities: capabilities), transport: reader); XCTFail("Lost chunk reply") }
            catch { XCTAssertEqual(error as? RunnerFixtureError, .disconnected) }
            let content = try await library.content(job.content)!
            try await library.put(LibraryContent(id: content.id, kind: content.kind, length: content.length,
                title: "Renamed", originalFilename: "different.zip"))
            let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
            let retry = TransferRunner(library: reopened, vault: vault)
            do { _ = try await retry.run(job.id, device: device(capabilities: capabilities), transport: reader); XCTFail("Lost commit reply") }
            catch { XCTAssertEqual(error as? RunnerFixtureError, .disconnected) }
            let durable = try await reopened.job(job.id)
            XCTAssertEqual(durable?.phase, .committing)
            let freshLibrary = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
            let freshRunner = TransferRunner(library: freshLibrary, vault: vault)
            let completed = try await freshRunner.run(job.id, device: device(capabilities: capabilities), transport: reader)
            XCTAssertEqual(completed.phase, .completed)
            let paths = await reader.declaredDestinations()
            XCTAssertEqual(paths.count, 3)
            XCTAssertTrue(paths.allSatisfy { $0 == "/dictionaries/\(job.content.hex)/dictionary" })
            let declarations = await reader.declaredBegins()
            XCTAssertEqual(declarations.count, 3)
            XCTAssertTrue(declarations.allSatisfy { $0.manifest.kind == .dictionary && $0.manifest.formatVersion == 1 })
            let received = await reader.bytes()
            XCTAssertEqual(received, try Data(contentsOf: root.appendingPathComponent("Dictionary.zip")))
            _ = try await freshLibrary.setReaderSelection(reader: job.reader, content: job.content, selected: true)
            let installed = declarations[0].manifest
            let inventory = try ReaderInventory(reader: job.reader, generation: job.storageGeneration,
                contents: [installed], complete: true)
            let work = try await freshLibrary.prepareContentWork(reader: job.reader, generation: job.storageGeneration,
                installation: job.installation, inventory: inventory)
            XCTAssertTrue(work.isEmpty, "Verified original ZIP inventory must not queue a duplicate install")
            for malformed in [
                try ContentManifest(content: installed.content, kind: .dictionary, length: installed.length,
                    formatVersion: 2, logicalIdentity: Data(count: 16)),
                try ContentManifest(content: installed.content, kind: .dictionary, length: installed.length,
                    formatVersion: 1, logicalIdentity: Data(repeating: 1, count: 16))
            ] {
                let invalidInventory = try ReaderInventory(reader: job.reader, generation: job.storageGeneration,
                    contents: [malformed], complete: true)
                do {
                    _ = try await freshLibrary.reconcileContent(reader: job.reader, generation: job.storageGeneration,
                        inventory: invalidInventory)
                    XCTFail("Malformed installed dictionary accepted")
                } catch { XCTAssertEqual(error as? ContentReconciliationError, .conflictingManifest(job.content)) }
            }
            _ = try await freshLibrary.setReaderSelection(reader: job.reader, content: job.content, selected: false)
            let removal = try await freshLibrary.prepareContentWork(reader: job.reader, generation: job.storageGeneration,
                installation: job.installation, inventory: inventory)
            XCTAssertEqual(removal, [.remove(installed)])
        }
    }
    private func setupFont(_ root: URL, vector: Bool = false) async throws -> (LibraryStore, ContentVault, TransferJob) {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let source = root.appendingPathComponent(vector ? "Fixture.ttf" : "Fixture_14.cpfont")
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { workspace.deleteLastPathComponent() }
        let fixture = workspace.appendingPathComponent("protocol/fixtures/" + (vector ? "VectorFont-sfnt.fixture" : "BitmapFont-v4.fixture"))
        try Data(contentsOf: fixture).write(to: source)
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let importer = ContentImporter(vault: vault, library: library)
        let content = vector ? try await importer.importVectorFont(source) : try await importer.importBitmapFont(source)
        let job = try await library.enqueue(content: content.id, reader: Data(repeating: 2, count: 16),
            storageGeneration: Data(repeating: 3, count: 16), installation: Data(repeating: 4, count: 16))
        return (library, vault, job)
    }
    func testFontTransferRetainsDestinationAcrossLostReplyAndDatabaseRestart() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job) = try await setupFont(root)
        let reader = ReaderTransferFixture(lostChunk: true)
        let capabilities: UInt32 = 1 | (1 << 5)
        let runner = TransferRunner(library: library, vault: vault)
        do { _ = try await runner.run(job.id, device: device(capabilities: capabilities), transport: reader); XCTFail("Lost reply") }
        catch { XCTAssertEqual(error as? RunnerFixtureError, .disconnected) }
        let retained = try await library.fontTransferPlan(job.id)
        XCTAssertEqual(retained.destination, "/fonts/Fixture/Fixture_14.cpfont")
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let retry = TransferRunner(library: reopened, vault: vault)
        let result = try await retry.run(job.id, device: device(capabilities: capabilities), transport: reader)
        XCTAssertEqual(result.phase, .completed)
        let paths = await reader.declaredDestinations()
        XCTAssertGreaterThanOrEqual(paths.count, 2)
        XCTAssertTrue(paths.allSatisfy { $0 == retained.destination })
        let planAgain = try await reopened.fontTransferPlan(job.id)
        XCTAssertEqual(planAgain, retained)
        let original = try await reopened.content(job.content)
        let renamed = LibraryContent(id: job.content, kind: .font, length: original!.length,
            title: "Renamed", originalFilename: "Other_14.cpfont")
        try await reopened.put(renamed)
        do {
            _ = try await reopened.prepareTransferDeclaration(job.id, verifiedLength: renamed.length)
            XCTFail("A retained job must not move to a different font destination")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        try await reopened.put(original!)
        let restored = try await reopened.fontTransferPlan(job.id)
        XCTAssertEqual(restored, retained)
    }
    func testVectorFontRequiresExplicitReaderCapabilityBeforeTraffic() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job) = try await setupFont(root, vector: true)
        let reader = ReaderTransferFixture()
        let runner = TransferRunner(library: library, vault: vault)
        for capabilities: UInt32 in [0, 1, 1 | (1 << 5)] {
            do { _ = try await runner.run(job.id, device: device(capabilities: capabilities), transport: reader); XCTFail("Unsupported font") }
            catch { XCTAssertEqual(error as? TransferRunnerError, .unsupportedContent) }
        }
        let count = await reader.count()
        XCTAssertEqual(count, 0)
        let retained = try await library.retainedTransferDeclaration(job.id)
        XCTAssertNil(retained)
        let result = try await runner.run(job.id, device: device(capabilities: 1 | (1 << 5) | (1 << 6)), transport: reader)
        XCTAssertEqual(result.phase, .completed)
        let paths = await reader.declaredDestinations()
        XCTAssertEqual(paths, ["/fonts/Fixture.ttf"])
    }
    func testCourseMetadataMustMatchVerifiedPackBeforeDeclarationOrTransmission() async throws {
        for assignment in ["edition=edition+1", "minor=minor+1", "locale='fr'"] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let (library, vault, job, _) = try await setupCourse(root)
            var database: OpaquePointer?
            XCTAssertEqual(sqlite3_open(root.appendingPathComponent("library.sqlite").path, &database), SQLITE_OK)
            XCTAssertEqual(sqlite3_exec(database, "UPDATE course_packs SET \(assignment)", nil, nil, nil), SQLITE_OK)
            XCTAssertEqual(sqlite3_close(database), SQLITE_OK)
            let reader = ReaderTransferFixture()
            let runner = TransferRunner(library: library, vault: vault)
            do {
                _ = try await runner.prepareDeclaration(job.id, device: device(capabilities: 3), installation: job.installation)
                XCTFail("Mismatched course metadata must not produce a declaration")
            } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
            let undeclared = try await library.job(job.id)
            XCTAssertEqual(undeclared, job)
            do {
                _ = try await runner.run(job.id, device: device(capabilities: 3), transport: reader)
                XCTFail("Mismatched course metadata must not reach the reader")
            } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
            let count = await reader.count()
            XCTAssertEqual(count, 0)
            let unchanged = try await library.job(job.id)
            XCTAssertEqual(unchanged?.id, job.id)
            XCTAssertEqual(unchanged?.content, job.content)
            XCTAssertEqual(unchanged?.storageGeneration, job.storageGeneration)
            XCTAssertEqual(unchanged?.durableOffset, 0)
            XCTAssertEqual(unchanged?.phase, .paused)
        }
    }
    func testRejectedCourseCommitCanBeAbortedAfterReaderConfirmsReceiving() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setupCourse(root)
        let reader = ReaderTransferFixture(rejectCommit: true)
        let runner = TransferRunner(library: library, vault: vault)
        do {
            _ = try await runner.run(job.id, device: device(capabilities: 3), transport: reader)
            XCTFail("reader rejection must stop the transfer")
        } catch { XCTAssertEqual(error as? TransferCommandError, .remote(.invalid)) }
        let failed = try await library.job(job.id)
        XCTAssertEqual(failed?.phase, .failed)
        let aborted = try await runner.abort(job.id, device: device(capabilities: 3), transport: reader)
        XCTAssertEqual(aborted.phase, .aborted)
    }
    func testRejectedCommitWithLostStatusRetainsCommitProtection() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setupCourse(root)
        let runner = TransferRunner(library: library, vault: vault)
        let reader = ReaderTransferFixture(rejectCommit: true, lostStatus: true)
        do {
            _ = try await runner.run(job.id, device: device(capabilities: 3),
                transport: reader)
            XCTFail("reader rejection must stop the transfer")
        } catch { XCTAssertEqual(error as? TransferCommandError, .remote(.invalid)) }
        let pending = try await library.job(job.id)
        XCTAssertEqual(pending?.phase, .committing)
        do { try await library.requestTransferAbort(job.id); XCTFail("unknown installation must remain protected") }
        catch { XCTAssertEqual(error as? StoreError, .invalidTransition) }
        await reader.restoreStatusConnection()
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let resumed = TransferRunner(library: reopened, vault: vault)
        do {
            _ = try await resumed.run(job.id, device: device(capabilities: 3), transport: reader)
            XCTFail("the resumed rejected pack must still report rejection")
        } catch { XCTAssertEqual(error as? TransferCommandError, .remote(.invalid)) }
        let confirmed = try await reopened.job(job.id)
        XCTAssertEqual(confirmed?.phase, .failed)
        let cancelled = try await resumed.abort(job.id, device: device(capabilities: 3), transport: reader)
        XCTAssertEqual(cancelled.phase, .aborted)
    }
    func testRejectedCommitStatusMustConfirmSafePhaseAndIdentity() async throws {
        for (phase, foreign) in [(TransferPhase.verified, false), (.installing, false),
                                (.committed, false), (.aborted, false), (.verified, true)] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let (library, vault, job, _) = try await setupCourse(root)
            let reader = ReaderTransferFixture(rejectCommit: true, rejectionStatus: phase, foreignStatus: foreign)
            let runner = TransferRunner(library: library, vault: vault)
            do {
                _ = try await runner.run(job.id, device: device(capabilities: 3), transport: reader)
                XCTFail("rejection must be reported")
            } catch { XCTAssertEqual(error as? TransferCommandError, .remote(.invalid)) }
            let pending = try await library.job(job.id)
            let safe = phase == .verified && !foreign
            XCTAssertEqual(pending?.phase, safe ? .failed : .committing)
            if safe {
                try await library.requestTransferAbort(job.id)
            } else {
                do { try await library.requestTransferAbort(job.id); XCTFail("installation must remain protected") }
                catch { XCTAssertEqual(error as? StoreError, .invalidTransition) }
            }
        }
    }
    func testCourseRequiresBothCapabilitiesBeforePreparingOrSending() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setupCourse(root)
        let reader = ReaderTransferFixture()
        let runner = TransferRunner(library: library, vault: vault)
        for capabilities: UInt32 in [0, 1, 2, 4, UInt32.max & ~UInt32(3)] {
            do {
                _ = try await runner.prepareDeclaration(job.id, device: device(capabilities: capabilities), installation: job.installation)
                XCTFail("missing capabilities must not retain a declaration")
            } catch { XCTAssertEqual(error as? TransferRunnerError, .unsupportedContent) }
            do {
                _ = try await runner.run(job.id, device: device(capabilities: capabilities), transport: reader)
                XCTFail("missing capabilities must leave course work pending")
            } catch { XCTAssertEqual(error as? TransferRunnerError, .unsupportedContent) }
        }
        let declarations = await reader.declaredBegins()
        XCTAssertTrue(declarations.isEmpty)
        let retained = try await library.retainedTransferDeclaration(job.id)
        XCTAssertNil(retained)
        let paused = try await library.job(job.id)
        XCTAssertEqual(paused?.phase, .paused)
        XCTAssertEqual(paused?.durableOffset, 0)
    }
    func testCourseConfirmationIsRequiredBeforeDeclaredBegin() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, bytes) = try await setupCourse(root, confirm: false)
        let reader = ReaderTransferFixture()
        let runner = TransferRunner(library: library, vault: vault)
        do {
            _ = try await runner.run(job.id, device: device(capabilities: 3), transport: reader)
            XCTFail("unconfirmed course must not transfer")
        } catch { XCTAssertEqual(error as? StoreError, .missingContent) }
        let before = await reader.declaredBegins()
        XCTAssertTrue(before.isEmpty)
        _ = try await library.associateCourse(job.content, confirmedIdentity: Data(repeating: 7, count: 16))
        let completed = try await runner.run(job.id, device: device(capabilities: 3), transport: reader)
        XCTAssertEqual(completed.phase, .completed)
        XCTAssertEqual(completed.durableOffset, UInt64(bytes.count))
        let declarations = await reader.declaredBegins()
        XCTAssertEqual(declarations.count, 1)
        XCTAssertEqual(declarations.first?.manifest.logicalIdentity, Data(repeating: 7, count: 16))
    }
    func testCourseTransferUsesEncryptedTransportAndResumesOverBluetoothAfterLostReply() async throws {
        for lostReply in [false, true] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let (library, vault, job, bytes) = try await setupCourse(root)
            let transaction = withUnsafeBytes(of: job.id.uuid) { Data($0) }
            var encoded = Data([1])
            for identity in [job.reader, job.storageGeneration, job.installation, transaction, Data(repeating: 5, count: 16)] {
                encoded.append(identity)
            }
            encoded.append(Data(0..<32)); encoded.append(contentsOf: [192, 168, 4, 1])
            encoded.appendLittleEndian(8080, count: 2); encoded.appendLittleEndian(30, count: 2)
            let offer = try WifiHandoffOffer(decoding: encoded)
            let reader = ReaderTransferFixture(lostChunk: lostReply)
            let wire = try EncryptedReaderTransferFixture(reader: reader, offer: offer)
            let wifi = try WifiHandoffTransport(offer: offer, reader: job.reader, storageGeneration: job.storageGeneration,
                installation: job.installation, transaction: transaction, receivedAt: 0, wire: wire, now: { 0 })
            let runner = TransferRunner(library: library, vault: vault)
            if lostReply {
                do {
                    _ = try await runner.run(job.id, device: device(capabilities: 3), transport: wifi)
                    XCTFail("lost encrypted reply must pause")
                } catch { XCTAssertEqual(error as? RunnerFixtureError, .disconnected) }
                let paused = try await library.job(job.id)
                XCTAssertEqual(paused?.phase, .paused)
                let closed = await wire.isClosed()
                XCTAssertTrue(closed)
                let retained = try await library.retainedTransferDeclaration(job.id)
                let restarted = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
                let resume = TransferRunner(library: restarted, vault: vault)
                let final = try await resume.run(job.id, device: device(capabilities: 3), transport: reader)
                XCTAssertEqual(final.phase, .completed)
                let declarations = await reader.declaredBegins()
                XCTAssertEqual(declarations.count, 2)
                XCTAssertTrue(declarations.allSatisfy { $0 == retained })
            } else {
                let final = try await runner.run(job.id, device: device(capabilities: 3), transport: wifi)
                XCTAssertEqual(final.phase, .completed)
                XCTAssertEqual(final.durableOffset, UInt64(bytes.count))
            }
            let received = await reader.bytes()
            XCTAssertEqual(received, bytes)
            let commits = await reader.commits()
            XCTAssertEqual(commits, 1)
        }
    }
    func testCourseRestartsWithIdenticalDeclarationAfterLostChunkAndCommit() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, bytes) = try await setupCourse(root)
        let reader = ReaderTransferFixture(lostChunk: true, lostCommit: true)
        let runner = TransferRunner(library: library, vault: vault)
        do {
            _ = try await runner.run(job.id, device: device(capabilities: 3), transport: reader)
            XCTFail("lost chunk reply must pause")
        } catch {}
        let retained = try await library.retainedTransferDeclaration(job.id)
        XCTAssertNotNil(retained)
        let restarted = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let resume = TransferRunner(library: restarted, vault: vault)
        do {
            _ = try await resume.run(job.id, device: device(capabilities: 3), transport: reader)
            XCTFail("lost commit reply must retain commit intent")
        } catch {}
        let committing = try await restarted.job(job.id)
        XCTAssertEqual(committing?.phase, .committing)
        let final = try await resume.run(job.id, device: device(capabilities: 3), transport: reader)
        XCTAssertEqual(final.phase, .completed)
        XCTAssertEqual(final.durableOffset, UInt64(bytes.count))
        let declarations = await reader.declaredBegins()
        XCTAssertEqual(declarations.count, 3)
        XCTAssertTrue(declarations.allSatisfy { $0 == retained })
    }
    func testLostAbortReplyKeepsIntentAcrossRestartAndBlocksResume() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setup(root)
        let reader = ReaderTransferFixture(lostChunk: true, lostAbort: true)
        let runner = TransferRunner(library: library, vault: vault)
        do { _ = try await runner.run(job.id, device: device(), transport: reader); XCTFail() } catch {}
        do { _ = try await runner.abort(job.id, device: device(), transport: reader); XCTFail("Lost abort ignored") } catch {}
        let restarted = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let intent = try await restarted.hasTransferAbort(job.id); XCTAssertTrue(intent)
        try await restarted.checkpoint(job.id, offset: 0, phase: .failed)
        let failedValue = try await restarted.job(job.id), failed = try XCTUnwrap(failedValue)
        let inventory = try ReaderInventory(reader: job.reader, generation: job.storageGeneration, contents: [], complete: true)
        let work = try await restarted.reconcileContentWork(reader: job.reader, generation: job.storageGeneration,
            installation: job.installation, inventory: inventory)
        XCTAssertEqual(work, [.abort(failed)])
        let recovery = TransferRunner(library: restarted, vault: vault)
        do { _ = try await recovery.run(job.id, device: device(), transport: reader); XCTFail("Aborting transfer resumed") }
        catch { XCTAssertEqual(error as? TransferRunnerError, .abortPending) }
        let aborted = try await recovery.abort(job.id, device: device(), transport: reader)
        XCTAssertEqual(aborted.phase, .aborted); XCTAssertEqual(aborted.durableOffset, 1000)
        let repeatAbort = try await recovery.abort(job.id, device: device(), transport: reader)
        XCTAssertEqual(repeatAbort, aborted)
    }
    func testDeselectionDuringStreamingAndAtLastChunkNeverCommits() async throws {
        for threshold: UInt64 in [1000, 4103] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let (library, vault, job, _) = try await setup(root)
            _ = try await library.setReaderSelection(reader: job.reader, content: job.content, selected: true)
            let reader = ReaderTransferFixture(deselectAtOffset: (library, job.reader, job.content, threshold))
            let runner = TransferRunner(library: library, vault: vault)
            do { _ = try await runner.run(job.id, device: device(), transport: reader, requireSelection: true); XCTFail("Deselected content committed") }
            catch {
                if threshold == 1000 { XCTAssertEqual(error as? TransferRunnerError, .deselected) }
                else { XCTAssertEqual(error as? StoreError, .invalidTransition) }
            }
            let paused = try await library.job(job.id)
            XCTAssertEqual(paused?.phase, .paused); XCTAssertEqual(paused?.durableOffset, threshold)
            let commits = await reader.commits(); XCTAssertEqual(commits, 0)
            let aborted = try await runner.abort(job.id, device: device(), transport: reader)
            XCTAssertEqual(aborted.phase, .aborted); XCTAssertEqual(aborted.durableOffset, threshold)
        }
    }
    func testAbortWithoutReaderTransactionAndCommitGuard() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setup(root)
        let reader = ReaderTransferFixture()
        let runner = TransferRunner(library: library, vault: vault)
        let aborted = try await runner.abort(job.id, device: device(), transport: reader)
        XCTAssertEqual(aborted.phase, .aborted); XCTAssertEqual(aborted.durableOffset, 0)
        let requests = await reader.count(); XCTAssertEqual(requests, 1)
        _ = try await runner.abort(job.id, device: device(), transport: reader)
        let repeated = await reader.count(); XCTAssertEqual(repeated, 1)
        let pending = try await library.enqueue(content: job.content, reader: job.reader,
            storageGeneration: job.storageGeneration, installation: job.installation)
        try await library.checkpoint(pending.id, offset: 4103, phase: .committing)
        do { _ = try await runner.abort(pending.id, device: device(), transport: reader); XCTFail("Committing job aborted") }
        catch { XCTAssertEqual(error as? StoreError, .invalidTransition) }
        let unchanged = await reader.count(); XCTAssertEqual(unchanged, 1)
        let noIntent = try await library.hasTransferAbort(pending.id); XCTAssertFalse(noIntent)
    }
    func testSelectedTransferRejectsDeselectionAndRecoversPriorCommit() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setup(root)
        let reader = ReaderTransferFixture(lostCommit: true)
        let runner = TransferRunner(library: library, vault: vault)
        do { _ = try await runner.run(job.id, device: device(), transport: reader, requireSelection: true); XCTFail("Unselected transfer started") }
        catch { XCTAssertEqual(error as? TransferRunnerError, .deselected) }
        let initialCount = await reader.count(); XCTAssertEqual(initialCount, 0)
        _ = try await library.setReaderSelection(reader: job.reader, content: job.content, selected: true)
        do { try await library.commitSelectedTransfer(job.id); XCTFail("Incomplete job entered commit") }
        catch { XCTAssertEqual(error as? StoreError, .invalidTransition) }
        do { _ = try await runner.run(job.id, device: device(), transport: reader, requireSelection: true); XCTFail("Lost commit ignored") }
        catch {}
        let committing = try await library.job(job.id); XCTAssertEqual(committing?.phase, .committing)
        _ = try await library.setReaderSelection(reader: job.reader, content: job.content, selected: false)
        let recovered = try await runner.run(job.id, device: device(), transport: reader, requireSelection: true)
        XCTAssertEqual(recovered.phase, .completed)
    }
    func testLostChunkResponseResumesAtReaderDurableOffset() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, bytes) = try await setup(root)
        let reader = ReaderTransferFixture(lostChunk: true)
        let runner = TransferRunner(library: library, vault: vault)
        do { _ = try await runner.run(job.id, device: device(), transport: reader); XCTFail("Disconnect ignored") } catch {}
        let paused = try await library.job(job.id)
        XCTAssertEqual(paused?.phase, .paused)
        XCTAssertEqual(paused?.durableOffset, 0)
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let resumed = TransferRunner(library: reopened, vault: vault)
        let finished = try await resumed.run(job.id, device: device(), transport: reader)
        XCTAssertEqual(finished.phase, .completed)
        XCTAssertEqual(finished.durableOffset, UInt64(bytes.count))
        let received = await reader.bytes()
        XCTAssertEqual(received, bytes)
    }
    func testLostCommitReplyReconcilesWithoutResendingContent() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, bytes) = try await setup(root)
        let reader = ReaderTransferFixture(lostCommit: true)
        let runner = TransferRunner(library: library, vault: vault)
        do { _ = try await runner.run(job.id, device: device(), transport: reader); XCTFail("Disconnect ignored") } catch {}
        let uncertain = try await library.job(job.id)
        XCTAssertEqual(uncertain?.phase, .committing)
        let finished = try await runner.run(job.id, device: device(), transport: reader)
        XCTAssertEqual(finished.phase, .completed)
        let received = await reader.bytes()
        XCTAssertEqual(received, bytes)
        let calls = await reader.count()
        _ = try await runner.run(job.id, device: device(), transport: reader)
        let after = await reader.count()
        XCTAssertEqual(after, calls)
    }
    func testWrongStorageAndUnacknowledgedChunkCannotCompleteJob() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setup(root)
        let reader = ReaderTransferFixture(badOffset: true)
        let runner = TransferRunner(library: library, vault: vault)
        do { _ = try await runner.run(job.id, device: device(generation: 9), transport: reader); XCTFail("Wrong storage accepted") }
        catch { XCTAssertEqual(error as? TransferRunnerError, .wrongStorage) }
        let calls = await reader.count()
        XCTAssertEqual(calls, 0)
        do { _ = try await runner.run(job.id, device: device(), transport: reader); XCTFail("Bad acknowledgement accepted") }
        catch { XCTAssertEqual(error as? TransferRunnerError, .invalidOffset) }
        let paused = try await library.job(job.id)
        XCTAssertEqual(paused?.phase, .paused)
        XCTAssertEqual(paused?.durableOffset, 0)
    }
    func testCancellationPreservesKnownInstallingCommit() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setup(root)
        let reader = ReaderTransferFixture(lostCommit: true, installingCommit: true, cancelInstallingBegin: true)
        let runner = TransferRunner(library: library, vault: vault)
        do { _ = try await runner.run(job.id, device: device(), transport: reader); XCTFail("Disconnect ignored") } catch {}
        let device = try device()
        let retry = Task { try await runner.run(job.id, device: device, transport: reader) }
        do { _ = try await retry.value; XCTFail("Cancellation ignored") }
        catch { XCTAssertTrue(error is CancellationError) }
        let uncertain = try await library.job(job.id)
        XCTAssertEqual(uncertain?.phase, .committing)
    }
    func testCancellationAfterCommitEffectsRetainsRecoveryAcrossRestart() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setup(root)
        let reader = ReaderTransferFixture(cancelCommit: true)
        let runner = TransferRunner(library: library, vault: vault)
        let task = Task { try await runner.run(job.id, device: device(), transport: reader) }
        do { _ = try await task.value; XCTFail("Commit acknowledgement cancellation ignored") }
        catch { XCTAssertTrue(error is CancellationError) }
        let restarted = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let pendingValue = try await restarted.job(job.id)
        let pending = try XCTUnwrap(pendingValue)
        XCTAssertEqual(pending.phase, .committing)
        let contentValue = try await restarted.content(job.content)
        let content = try XCTUnwrap(contentValue)
        XCTAssertEqual(pending.durableOffset, content.length)
        let received = await reader.bytes()
        let commitCount = await reader.commits()
        XCTAssertEqual(commitCount, 1)
        let resumed = try await TransferRunner(library: restarted, vault: vault)
            .run(job.id, device: device(), transport: reader)
        XCTAssertEqual(resumed.id, job.id)
        XCTAssertEqual(resumed.phase, .completed)
        let finalBytes = await reader.bytes()
        let finalCommits = await reader.commits()
        XCTAssertEqual(finalBytes, received)
        XCTAssertEqual(finalCommits, commitCount)
        let jobs = try await restarted.pendingJobs()
        XCTAssertTrue(jobs.isEmpty)
    }

    func testPauseAfterDurableChunkResumesSameJobAfterRestart() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setup(root)
        let reader = ReaderTransferFixture(cancelChunk: true)
        let runner = TransferRunner(library: library, vault: vault)
        let transfer = Task { try await runner.run(job.id, device: device(), transport: reader) }
        do { _ = try await transfer.value; XCTFail("Cancellation ignored") }
        catch { XCTAssertTrue(error is CancellationError) }
        let restarted = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let pausedValue = try await restarted.job(job.id)
        let paused = try XCTUnwrap(pausedValue)
        XCTAssertEqual(paused.phase, .paused)
        XCTAssertEqual(paused.durableOffset, 1000)
        let aborted = try await restarted.hasTransferAbort(job.id); XCTAssertFalse(aborted)
        let resumed = try await TransferRunner(library: restarted, vault: vault)
            .run(job.id, device: device(), transport: reader)
        XCTAssertEqual(resumed.id, job.id)
        XCTAssertEqual(resumed.phase, .completed)
        let pending = try await restarted.pendingJobs(); XCTAssertTrue(pending.isEmpty)
    }

    func testHandoffPreparationStagesLargeJobWithoutChunksOrCommitAndSurvivesRestart() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setup(root, count: 1024 * 1024 + 1)
        _ = try await library.setReaderSelection(reader: job.reader, content: job.content, selected: true)
        let reader = ReaderTransferFixture(), runner = TransferRunner(library: library, vault: vault)
        let prepared = try await runner.run(job.id, device: device(), transport: reader, requireSelection: true, prepareOnly: true)
        XCTAssertEqual(prepared.phase, .paused); XCTAssertEqual(prepared.durableOffset, 0)
        let calls = await reader.count(), bytes = await reader.bytes(), commits = await reader.commits()
        XCTAssertEqual(calls, 1); XCTAssertTrue(bytes.isEmpty); XCTAssertEqual(commits, 0)
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let restored = try await reopened.job(job.id)
        XCTAssertEqual(restored, prepared)
        let repeated = try await runner.run(job.id, device: device(), transport: reader, requireSelection: true, prepareOnly: true)
        XCTAssertEqual(repeated, prepared)
        let repeatedBytes = await reader.bytes(); XCTAssertTrue(repeatedBytes.isEmpty)
    }
    func testHandoffPreparationRejectsSmallUnselectedAndCommitProtectedJobsBeforeBle() async throws {
        for scenario in 0..<3 {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let (library, vault, job, bytes) = try await setup(root, count: scenario == 0 ? 4103 : 1024 * 1024 + 1)
            if scenario != 1 { _ = try await library.setReaderSelection(reader: job.reader, content: job.content, selected: true) }
            if scenario == 2 { try await library.checkpoint(job.id, offset: UInt64(bytes.count), phase: .committing) }
            let reader = ReaderTransferFixture(), runner = TransferRunner(library: library, vault: vault)
            do { _ = try await runner.run(job.id, device: device(), transport: reader, requireSelection: true, prepareOnly: true); XCTFail() }
            catch { XCTAssertEqual(error as? TransferRunnerError, scenario == 1 ? .deselected : .handoffUnavailable) }
            let calls = await reader.count(); XCTAssertEqual(calls, 0)
            if scenario == 2 {
                let retained = try await library.job(job.id); XCTAssertEqual(retained?.phase, .committing)
            }
        }
    }

    func testHandoffPreparationReconcilesReaderCommitAndProtectsInstallation() async throws {
        for phase: TransferPhase in [.installing, .committed] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let (library, vault, job, _) = try await setup(root, count: 1024 * 1024 + 1)
            _ = try await library.setReaderSelection(reader: job.reader, content: job.content, selected: true)
            let reader = ReaderTransferFixture(beginPhase: phase), runner = TransferRunner(library: library, vault: vault)
            if phase == .installing {
                do { _ = try await runner.run(job.id, device: device(), transport: reader, requireSelection: true, prepareOnly: true); XCTFail() }
                catch { XCTAssertEqual(error as? TransferRunnerError, .handoffUnavailable) }
                let protected = try await library.job(job.id); XCTAssertEqual(protected?.phase, .committing)
            } else {
                let completed = try await runner.run(job.id, device: device(), transport: reader, requireSelection: true, prepareOnly: true)
                XCTAssertEqual(completed.phase, .completed)
            }
            let calls = await reader.count(), bytes = await reader.bytes(), commits = await reader.commits()
            XCTAssertEqual(calls, 1); XCTAssertTrue(bytes.isEmpty); XCTAssertEqual(commits, 0)
        }
    }
    func testHandoffPreparationUsesReaderOffsetAfterLostBleChunkReply() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setup(root, count: 1024 * 1024 + 1)
        _ = try await library.setReaderSelection(reader: job.reader, content: job.content, selected: true)
        let reader = ReaderTransferFixture(lostChunk: true), runner = TransferRunner(library: library, vault: vault)
        do { _ = try await runner.run(job.id, device: device(), transport: reader); XCTFail() }
        catch RunnerFixtureError.disconnected {} catch { XCTFail("Unexpected error: \(error)") }
        let before = await reader.bytes(), count = await reader.count()
        XCTAssertFalse(before.isEmpty)
        let prepared = try await runner.run(job.id, device: device(), transport: reader, requireSelection: true, prepareOnly: true)
        XCTAssertEqual(prepared.phase, .paused); XCTAssertEqual(prepared.durableOffset, UInt64(before.count))
        let after = await reader.bytes(), afterCount = await reader.count()
        XCTAssertEqual(after, before); XCTAssertEqual(afterCount, count + 1)
    }

    func testRemovedCourseStillRequiresImmutableExplicitSwitchConsent() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setupCourse(root)
        let old = try ContentManifest(content: ContentID(String(repeating: "a", count: 64)), kind: .course,
            length: 100, formatVersion: 1, logicalIdentity: Data(repeating: 9, count: 16))
        let context = try ReaderCourseContext(generation: job.storageGeneration, source: .removed, manifest: old)
        let inventory = try ReaderInventory(reader: job.reader, generation: job.storageGeneration,
            contents: [], complete: true, courseContext: context)
        do { _ = try await library.admitCourseTransfer(job.content, inventory: inventory, vault: vault); XCTFail() }
        catch CourseTransferAdmissionError.differentCourse {}
        let queued = try await library.queueCourseSwitch(content: job.content, inventory: inventory, installation: job.installation)
        XCTAssertEqual(queued.id, job.id)
        let savedConsent = try await library.courseSwitchConfirmation(job.id)
        let consent = try XCTUnwrap(savedConsent)
        XCTAssertEqual(consent.previousCourse, old.logicalIdentity)
        XCTAssertEqual(consent.previousHash, old.content.digest)
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let admission = try await reopened.admitCourseTransfer(job.content, inventory: inventory,
            vault: vault, confirmedSwitchJob: job.id)
        XCTAssertEqual(admission, .explicitSwitch)
        let foreign = try ContentManifest(content: ContentID(String(repeating: "b", count: 64)), kind: .course,
            length: old.length, formatVersion: 1, logicalIdentity: old.logicalIdentity)
        let changed = try ReaderInventory(reader: job.reader, generation: job.storageGeneration, contents: [], complete: true,
            courseContext: ReaderCourseContext(generation: job.storageGeneration, source: .removed, manifest: foreign))
        do { _ = try await reopened.confirmCourseSwitch(job.id, inventory: changed); XCTFail() }
        catch StoreError.conflictingJob {}
        let retained = try await reopened.courseSwitchConfirmation(job.id)
        XCTAssertEqual(retained, consent)
    }
    func testConfirmedSwitchSurvivesRestartAndResendsConsentBeforeResume() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let (library, vault, job, _) = try await setupCourse(root)
        let old = try ContentManifest(content: ContentID(String(repeating: "a", count: 64)), kind: .course,
            length: 100, formatVersion: 1, logicalIdentity: Data(repeating: 9, count: 16))
        let inventory = try ReaderInventory(reader: job.reader, generation: job.storageGeneration, contents: [old], complete: true)
        let consent = try await library.confirmCourseSwitch(job.id, inventory: inventory)
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let saved = try await reopened.courseSwitchConfirmation(job.id)
        XCTAssertEqual(saved, consent)
        let candidateObject = try await vault.verifiedObject(job.content)
        let metadata = try CoursePackInspector.inspect(candidateObject.url)
        try await reopened.putCoursePack(LibraryContent(id: old.content, kind: .course, length: old.length,
            title: "Old course", originalFilename: "old.pack", languages: [metadata.locale]), metadata: metadata)
        let book = try ContentID(String(repeating: "c", count: 64))
        try await reopened.put(LibraryContent(id: book, kind: .epub, length: 10, title: "Book", originalFilename: "book.epub"))
        _ = try await reopened.setReaderSelection(reader: job.reader, content: old.content, selected: true)
        _ = try await reopened.setReaderSelection(reader: job.reader, content: book, selected: true)
        _ = try await reopened.setReaderSelection(reader: job.reader, content: job.content, selected: false)
        let queued = try await reopened.queueCourseSwitch(content: job.content, inventory: inventory, installation: job.installation)
        XCTAssertEqual(queued.id, job.id)
        let choices = try await reopened.readerSelections(reader: job.reader)
        XCTAssertEqual(Dictionary(uniqueKeysWithValues: choices.map { ($0.content, $0.selected) }),
            [job.content: true, old.content: false, book: true])
        let admission = try await reopened.admitCourseTransfer(job.content, inventory: inventory, vault: vault, confirmedSwitchJob: job.id)
        XCTAssertEqual(admission, .explicitSwitch)
        do {
            _ = try await reopened.admitCourseTransfer(job.content, inventory: inventory, vault: vault)
            XCTFail("ordinary transfer must still reject another course")
        } catch CourseTransferAdmissionError.differentCourse {}
        let foreign = try ContentManifest(content: ContentID(String(repeating: "b", count: 64)), kind: .course,
            length: 100, formatVersion: 1, logicalIdentity: old.logicalIdentity)
        do {
            _ = try await reopened.confirmCourseSwitch(job.id, inventory: ReaderInventory(
                reader: job.reader, generation: job.storageGeneration, contents: [foreign], complete: true))
            XCTFail("consent cannot retarget old pack")
        } catch StoreError.conflictingJob {}
        do {
            _ = try await reopened.queueCourseSwitch(content: job.content, inventory: ReaderInventory(
                reader: job.reader, generation: job.storageGeneration, contents: [foreign], complete: true),
                installation: job.installation)
            XCTFail("queue confirmation must not retarget")
        } catch StoreError.conflictingJob {}
        let preservedChoices = try await reopened.readerSelections(reader: job.reader)
        XCTAssertEqual(preservedChoices, choices)
        let preservedConsent = try await reopened.courseSwitchConfirmation(job.id)
        XCTAssertEqual(preservedConsent, consent)
        let reader = ReaderTransferFixture(lostChunk: true)
        let runner = TransferRunner(library: reopened, vault: vault)
        do {
            _ = try await runner.prepareDeclaration(job.id, device: device(capabilities: 3), installation: job.installation)
            XCTFail("switch capability required before handoff declaration")
        } catch TransferRunnerError.unsupportedContent {}
        let rejectedDeclaration = try await reopened.retainedTransferDeclaration(job.id)
        XCTAssertNil(rejectedDeclaration)
        do {
            _ = try await runner.run(job.id, device: device(capabilities: 3), transport: reader)
            XCTFail("switch capability required")
        } catch TransferRunnerError.unsupportedContent {}
        let noTraffic = await reader.count()
        XCTAssertEqual(noTraffic, 0)
        do {
            _ = try await runner.run(job.id, device: device(capabilities: 19), transport: reader)
            XCTFail("lost chunk must pause")
        } catch RunnerFixtureError.disconnected {}
        let restarted = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let resume = TransferRunner(library: restarted, vault: vault)
        let result = try await resume.run(job.id, device: device(capabilities: 19), transport: reader)
        XCTAssertEqual(result.phase, .completed)
        let sent = await reader.savedConsents()
        XCTAssertEqual(sent, [consent, consent])
        _ = try await resume.run(job.id, device: device(capabilities: 19), transport: reader)
        let repeated = await reader.savedConsents()
        XCTAssertEqual(repeated, sent)
    }

    func testFirmwareStagingResumesLostRepliesAndRetainsStagingReceipt() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        var image = Data(count: 24)
        image[0] = 0xe9; image[1] = 1; image[12] = 5; image[23] = 1
        var payload = Data(repeating: 0x5a, count: 1024 * 1024 + 1)
        let tag = Data("CROSSPOINT-BOARD-V1:x4;".utf8)
        payload.replaceSubrange(0..<tag.count, with: tag)
        image.append(Data(count: 4)); image.appendLittleEndian(UInt64(payload.count), count: 4)
        image.append(payload)
        var padding = Data(count: ((image.count + 16) & ~15) - image.count)
        padding[padding.count-1] = payload.reduce(UInt8(0xef), ^)
        image.append(padding)
        image.append(contentsOf: SHA256.hash(data: image))
        let source = root.appendingPathComponent("update.bin")
        try image.write(to: source)
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let object = try await vault.importFile(source)
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let metadata: [String: Any] = ["kind": "firmware", "sha256": object.id.hex, "length": object.length,
            "boardTags": ["x4"], "chipId": 5, "otaPartitionBytes": 0x900000, "minimumBatteryPercent": 30,
            "stateSchema": ["minimum": 1, "maximum": 1], "companionProtocol": ["minimum": 1, "maximum": 1],
            "initialUpgradeRequired": false, "supportedJournalHeaderVersions": [1,2,3]]
        try await library.putFirmware(LibraryContent(id: object.id, kind: .firmware, length: object.length,
            title: "Update", originalFilename: "update.bin"), releaseAsset: JSONSerialization.data(withJSONObject: metadata))
        var descriptorBytes = try device().encoded
        descriptorBytes.replaceSubrange(42..<74, with: Data(repeating: 2, count: 32))
        let descriptor = try DeviceDescriptor(decoding: descriptorBytes)
        var infoBytes = Data([0x46,0x57,0x49,1,1,60,1,1]) + descriptor.storageGeneration + descriptor.runningBuild
        infoBytes.appendLittleEndian(5, count: 2); infoBytes.append(Data(count: 2))
        infoBytes.appendLittleEndian(0x900000, count: 8)
        infoBytes.appendLittleEndian(1, count: 4); infoBytes.appendLittleEndian(3, count: 4)
        let info = try FirmwareReaderInfo(decoding: infoBytes)
        let job = try await library.enqueueFirmware(content: object.id, reader: descriptor.identity,
            storageGeneration: descriptor.storageGeneration, installation: Data(repeating: 4, count: 16))
        let duplicate = try await library.enqueueFirmware(content: object.id, reader: descriptor.identity,
            storageGeneration: descriptor.storageGeneration, installation: Data(repeating: 4, count: 16))
        XCTAssertEqual(duplicate.id, job.id)
        do {
            _ = try await library.enqueueFirmware(content: object.id, reader: descriptor.identity,
                storageGeneration: descriptor.storageGeneration, installation: Data(repeating: 9, count: 16))
            XCTFail("another installation cannot claim pending firmware")
        } catch StoreError.conflictingJob {}
        let reader = ReaderTransferFixture(lostChunk: true, lostCommit: true)
        let runner = TransferRunner(library: library, vault: vault)
        do { _ = try await runner.run(job.id, device: descriptor, transport: reader); XCTFail("firmware requires admission") }
        catch TransferRunnerError.unsupportedContent {}
        var lowBattery = infoBytes; lowBattery[5] = 29
        do {
            _ = try await runner.stageFirmware(job.id, device: descriptor,
                info: FirmwareReaderInfo(decoding: lowBattery), transport: reader)
            XCTFail("fresh battery admission")
        } catch FirmwareTransferAdmissionError.incompatibleReader {}
        let none = await reader.count()
        XCTAssertEqual(none, 0)
        let preparation = try await runner.prepareFirmwareHandoff(job.id, device: descriptor, info: info, transport: reader)
        XCTAssertEqual(preparation.job.phase, .paused)
        XCTAssertEqual(preparation.job.durableOffset, 0)
        let transaction = withUnsafeBytes(of: job.id.uuid) { Data($0) }
        var offerBytes = Data([1])
        for identity in [job.reader, job.storageGeneration, job.installation, transaction, Data(repeating: 5, count: 16)] {
            offerBytes.append(identity)
        }
        offerBytes.append(Data(0..<32)); offerBytes.append(contentsOf: [192, 168, 4, 1])
        offerBytes.appendLittleEndian(8080, count: 2); offerBytes.appendLittleEndian(30, count: 2)
        let offer = try WifiHandoffOffer(decoding: offerBytes)
        let wire = try EncryptedReaderTransferFixture(reader: reader, offer: offer)
        let wifi = try WifiHandoffTransport(offer: offer, reader: job.reader, storageGeneration: job.storageGeneration,
            installation: job.installation, transaction: transaction, receivedAt: 0, wire: wire, now: { 0 })
        do { _ = try await runner.stageFirmware(job.id, device: descriptor, info: info, transport: wifi); XCTFail("lost chunk") }
        catch RunnerFixtureError.disconnected {}
        do { _ = try await library.firmwareStagingReceipt(job.id); XCTFail("paused image is not staged") }
        catch StoreError.invalidTransition {}
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let resume = TransferRunner(library: reopened, vault: vault)
        let resumedJob = try await reopened.enqueueFirmware(content: object.id, reader: descriptor.identity,
            storageGeneration: descriptor.storageGeneration, installation: job.installation)
        XCTAssertEqual(resumedJob.id, job.id)
        do { _ = try await resume.stageFirmware(job.id, device: descriptor, info: info, transport: reader); XCTFail("lost commit") }
        catch RunnerFixtureError.disconnected {}
        let receipt = try await resume.stageFirmware(job.id, device: descriptor, info: info, transport: reader)
        XCTAssertEqual(receipt.image, object.id)
        XCTAssertEqual(receipt.length, object.length)
        XCTAssertEqual(receipt.transaction, job.id)
        let retainedRelease = try await reopened.firmwareCompatibility(object.id)
        let release = try XCTUnwrap(retainedRelease)
        let install = try FirmwareInstallRequest(receipt: receipt, metadata: release, device: descriptor, info: info)
        XCTAssertEqual(install.bytes.count, 104)
        XCTAssertEqual(try FirmwareInstallRequest(decoding: install.bytes), install)
        XCTAssertEqual(install.bytes.subdata(in: 20..<36), transaction)
        XCTAssertEqual(install.bytes.subdata(in: 36..<68), object.id.digest)
        XCTAssertThrowsError(try FirmwareInstallRequest(receipt: receipt, metadata: release, device: descriptor,
            info: FirmwareReaderInfo(decoding: lowBattery)))
        let installTransport = FirmwareInstallFixture()
        do {
            _ = try await resume.installFirmware(job.id, device: descriptor, installation: job.installation,
                info: info, transport: installTransport)
            XCTFail("lost acceptance")
        } catch RunnerFixtureError.disconnected {}
        let installationStore = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let waiting = try await installationStore.firmwareInstallation(job.id)
        XCTAssertEqual(waiting?.request, install)
        XCTAssertEqual(waiting?.bootVerified, false)
        let installRunner = TransferRunner(library: installationStore, vault: vault)
        let accepted = try await installRunner.installFirmware(job.id, device: descriptor, installation: job.installation,
            info: info, transport: installTransport)
        XCTAssertFalse(accepted.bootVerified)
        let oldBoot = try await installationStore.verifyFirmwareInstallation(job.id, device: descriptor, installation: job.installation)
        XCTAssertFalse(oldBoot)
        do {
            _ = try await installationStore.verifyFirmwareInstallation(job.id, device: descriptor,
                installation: Data(repeating: 9, count: 16))
            XCTFail("another installation cannot verify")
        } catch StoreError.conflictingJob {}
        var bootBytes = descriptor.encoded
        bootBytes.replaceSubrange(42..<74, with: object.id.digest)
        let booted = try DeviceDescriptor(decoding: bootBytes)
        var bootInfoBytes = infoBytes
        bootInfoBytes[5] = 29
        bootInfoBytes.replaceSubrange(24..<56, with: object.id.digest)
        let verified = try await installRunner.installFirmware(job.id, device: booted, installation: job.installation,
            info: FirmwareReaderInfo(decoding: bootInfoBytes), transport: installTransport)
        XCTAssertTrue(verified.bootVerified)
        let installCommands = await installTransport.count()
        XCTAssertEqual(installCommands, 2)
        let finalStore = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let finalInstallation = try await finalStore.firmwareInstallation(job.id)
        XCTAssertEqual(finalInstallation, verified)
        let stagedJobs = try await finalStore.stagedFirmwareJobs(reader: descriptor.identity,
            generation: descriptor.storageGeneration, installation: job.installation)
        XCTAssertEqual(stagedJobs.map(\.id), [job.id])
        let wrongGenerationJobs = try await finalStore.stagedFirmwareJobs(reader: descriptor.identity,
            generation: Data(repeating: 9, count: 16), installation: job.installation)
        XCTAssertTrue(wrongGenerationJobs.isEmpty)
        let installationHistory = try await finalStore.firmwareInstallations(reader: descriptor.identity, installation: job.installation)
        XCTAssertEqual(installationHistory, [verified])
        let foreignInstallationHistory = try await finalStore.firmwareInstallations(reader: descriptor.identity,
            installation: Data(repeating: 9, count: 16))
        XCTAssertTrue(foreignInstallationHistory.isEmpty)
        let persisted = try await reopened.firmwareStagingReceipt(job.id)
        XCTAssertEqual(persisted, receipt)
        let declarations = await reader.declaredBegins()
        XCTAssertEqual(declarations.count, 4)
        XCTAssertTrue(declarations.allSatisfy { $0 == declarations[0] && $0.manifest.kind == .firmware })
    }

}

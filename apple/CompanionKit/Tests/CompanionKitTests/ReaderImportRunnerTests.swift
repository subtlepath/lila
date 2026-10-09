import Foundation
import XCTest
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

private enum ImportReadError: Error { case disconnected }
private actor ImportReadFixture {
    let bytes: Data
    var offsets: [UInt64] = []
    var loseAt: UInt64?
    let badBinding: Bool
    let cancel: Bool
    let deletion: (LibraryStore, ContentID)?
    init(bytes: Data, loseAt: UInt64? = nil, badBinding: Bool = false, cancel: Bool = false,
         deletion: (LibraryStore, ContentID)? = nil) {
        self.bytes = bytes; self.loseAt = loseAt; self.badBinding = badBinding; self.cancel = cancel; self.deletion = deletion
        offsets.reserveCapacity(32)
    }
    func read(_ request: ReaderContentReadRequest) async throws -> Data {
        offsets.append(request.offset)
        if loseAt == request.offset { loseAt = nil; throw ImportReadError.disconnected }
        if let (library, content) = deletion {
            let change = try LibraryVisibilityChange(origin: Data(repeating: 8, count: 16), content: content, removed: true, ancestors: [])
            _ = try await library.importLibraryVisibilityChanges([change])
        }
        if cancel { withUnsafeCurrentTask { $0?.cancel() } }
        let count = Int(min(UInt64(request.maximumBytes), request.manifest.length - request.offset))
        var reply = Data([0x4c, 0x43, 0x53, 1, 0])
        reply.reserveCapacity(63 + count)
        reply.append(badBinding ? Data(repeating: 4, count: 16) : request.generation)
        reply.append(request.manifest.content.digest); reply.appendLittleEndian(request.offset, count: 8)
        reply.appendLittleEndian(UInt64(count), count: 2)
        reply.append(bytes.subdata(in: Int(request.offset) ..< Int(request.offset) + count))
        return reply
    }
    func requests() -> [UInt64] { offsets }
}

private final class ImportCredentialStorage: CredentialStorage, @unchecked Sendable {
    private let lock = NSLock()
    private var records: [String: Data] = [:]
    func load(_ account: String) throws -> Data? { lock.lock(); defer { lock.unlock() }; return records[account] }
    func insert(_ account: String, data: Data) throws -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard records[account] == nil else { return false }; records[account] = data; return true
    }
    func remove(_ account: String) throws { lock.lock(); defer { lock.unlock() }; records.removeValue(forKey: account) }
}
private actor AuthenticatedImportReader: SessionTransport {
    private let descriptor: Data
    private let source: ImportReadFixture
    private let wrongReplyID: Bool
    private let deletionOnHandoff: LibraryStore?
    private let cancelOnMetadata: (LibraryStore, UUID)?
    private var metadataRequests = 0
    private var connection: UInt64 = 1
    private var credential: Data?
    init(device: DeviceDescriptor, bytes: Data, supported: Bool = true, wrongReplyID: Bool = false,
         deletionOnHandoff: LibraryStore? = nil, cancelOnMetadata: (LibraryStore, UUID)? = nil) {
        var encoded = device.encoded; encoded[36] = supported ? 28 : 0
        descriptor = encoded; source = ImportReadFixture(bytes: bytes); self.wrongReplyID = wrongReplyID
        self.deletionOnHandoff = deletionOnHandoff; self.cancelOnMetadata = cancelOnMetadata
    }
    func reconnect() { connection += 1 }
    func sessionIdentity() async throws -> UInt64 { connection }
    func metadataCount() -> Int { metadataRequests }
    func count() async -> Int { await source.requests().count }
    func offsets() async -> [UInt64] { await source.requests() }
    func exchange(_ request: ControlFrame, connection: UInt64) async throws -> ControlFrame {
        guard connection == self.connection else { throw ReaderSessionError.staleConnection }
        return try await exchange(request)
    }
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        switch request.command {
        case .discover: return try ControlFrame(command: request.command, response: true, requestID: request.requestID, payload: descriptor)
        case .authenticateInstallation:
            return try ControlFrame(command: credential == request.payload ? request.command : .error, response: true,
                                    requestID: request.requestID, payload: Data([credential == request.payload ? 0 : 2]))
        case .registerInstallation:
            credential = request.payload
            return try ControlFrame(command: request.command, response: true, requestID: request.requestID, payload: Data([0]))
        case .contentMetadata:
            metadataRequests += 1
            let body = try ReaderContentMetadataRequest(decoding: request.payload)
            if let (library, id) = cancelOnMetadata { try await library.abortReaderImport(id) }
            let name = Data((body.manifest.kind == .epub ? "Book.epub" : "Spanish.pack").utf8)
            var payload = Data([0x4c, 0x43, 0x4e, 1, 0])
            payload.append(body.generation); payload.append(body.manifest.encoded)
            payload.append(UInt8(name.count)); payload.append(name)
            return try ControlFrame(command: request.command, response: true,
                requestID: request.requestID + (wrongReplyID ? 1 : 0), payload: payload)
        case .prepareContentHandoff:
            let body = try ReaderContentHandoffRequest(decoding: request.payload)
            if let deletionOnHandoff {
                let change = try LibraryVisibilityChange(origin: Data(repeating: 8, count: 16),
                    content: body.read.manifest.content, removed: true, ancestors: [])
                _ = try await deletionOnHandoff.importLibraryVisibilityChanges([change])
            }
            var payload = Data([0x4c, 0x43, 0x54, 1, 0])
            payload.append(body.transaction); payload.append(body.read.generation)
            payload.append(body.read.manifest.content.digest); payload.appendLittleEndian(body.read.offset, count: 8)
            return try ControlFrame(command: request.command, response: true,
                requestID: request.requestID + (wrongReplyID ? 1 : 0), payload: payload)
        case .readContent:
            let body = try ReaderContentReadRequest(decoding: request.payload)
            return try ControlFrame(command: request.command, response: true, requestID: request.requestID + (wrongReplyID ? 1 : 0),
                                    payload: await source.read(body))
        default: throw ProtocolError.command
        }
    }
}

private actor EncryptedImportReadFixture: WifiMessageTransport {
    private let cipher: WifiMessageCipher
    private let transaction: Data
    private let source: ImportReadFixture
    private var closed = false
    private var loseReplyAt: UInt64?
    init(offer: WifiHandoffOffer, bytes: Data, loseReplyAt: UInt64? = nil) throws {
        self.loseReplyAt = loseReplyAt
        cipher = try WifiMessageCipher(key: offer.key, session: offer.session, sending: .readerToApple)
        transaction = offer.transaction; source = ImportReadFixture(bytes: bytes)
    }
    func exchange(_ message: Data, timeoutNanoseconds: UInt64) async throws -> Data {
        let frame = try ControlFrame(decoding: await cipher.open(message), authenticated: true)
        guard frame.command == .readContent, frame.payload.prefix(16) == transaction else { throw ProtocolError.value }
        let request = try ReaderContentReadRequest(decoding: Data(frame.payload.dropFirst(16)))
        let reply = try ControlFrame(command: .readContent, response: true, requestID: frame.requestID,
                                     payload: await source.read(request))
        if loseReplyAt == request.offset { loseReplyAt = nil; throw ImportReadError.disconnected }
        return try await cipher.seal(reply.encoded())
    }
    func close() async { closed = true; await cipher.invalidate() }
    func isClosed() -> Bool { closed }
    func offsets() async -> [UInt64] { await source.requests() }
}

final class ReaderImportRunnerTests: XCTestCase, @unchecked Sendable {
    private struct Fixture {
        let root: URL, library: LibraryStore, storage: ReaderImportStorage, vault: ContentVault
        let runner: ReaderImportRunner, job: ReaderImportJob, bytes: Data
        let device: DeviceDescriptor, inventory: ReaderInventory
    }
    private func setup() async throws -> Fixture {
        var repository = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { repository.deleteLastPathComponent() }
        let bytes = try Data(contentsOf: repository.appendingPathComponent("test/tinta/fixtures/mini.pack"))
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let storage = try ReaderImportStorage(root: root.appendingPathComponent("reader-imports"))
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let hash = try ContentID(SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined())
        let manifest = try ContentManifest(content: hash, kind: .course, length: UInt64(bytes.count), formatVersion: 1,
                                           logicalIdentity: Data(repeating: 7, count: 16))
        var descriptor = Data([1, 1]); descriptor.append(Data(repeating: 1, count: 16)); descriptor.append(Data(repeating: 2, count: 16))
        descriptor.append(1); descriptor.appendLittleEndian(0, count: 4); descriptor.append(contentsOf: [80, 1, 1]); descriptor.append(Data(count: 32))
        let device = try DeviceDescriptor(decoding: descriptor)
        let inventory = try ReaderInventory(reader: device.identity, generation: device.storageGeneration, contents: [manifest], complete: true)
        let job = try await library.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: Data(repeating: 3, count: 16))
        let importer = ContentImporter(vault: vault, library: library)
        return Fixture(root: root, library: library, storage: storage, vault: vault,
                       runner: ReaderImportRunner(library: library, storage: storage, importer: importer),
                       job: job, bytes: bytes, device: device, inventory: inventory)
    }
    private func rejected(_ operation: () async throws -> Void) async {
        do { try await operation(); XCTFail("Invalid runner operation accepted") } catch {}
    }
    func testDownloadPublishesAndCompletedRetryDoesNotReadAgain() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let source = ImportReadFixture(bytes: f.bytes)
        let content = try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory,
            installation: f.job.installation, originalFilename: "Spanish.pack") { try await source.read($0) }
        XCTAssertEqual(content.id, f.job.manifest.content)
        let first = await source.requests(); XCTAssertEqual(first.first, 0)
        XCTAssertEqual(first.count, (f.bytes.count + 960) / 961)
        let completed = try await f.library.readerImportJob(f.job.id); XCTAssertEqual(completed?.phase, .completed)
        let selected = try await f.library.isReaderContentSelected(reader: f.job.reader, content: content.id); XCTAssertTrue(selected)
        _ = try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory,
            installation: f.job.installation, originalFilename: "Spanish.pack") { try await source.read($0) }
        let repeated = await source.requests(); XCTAssertEqual(repeated, first)
    }
    func testDisconnectPausesDurableOffsetAndRestartResumesNextChunk() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let source = ImportReadFixture(bytes: f.bytes, loseAt: 961)
        await rejected { _ = try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory,
            installation: f.job.installation, originalFilename: "Spanish.pack") { try await source.read($0) } }
        let paused = try await f.library.readerImportJob(f.job.id)
        XCTAssertEqual(paused?.phase, .paused); XCTAssertEqual(paused?.acknowledgedOffset, 961)
        let library = try LibraryStore(url: f.root.appendingPathComponent("library.sqlite"))
        let storage = try ReaderImportStorage(root: f.root.appendingPathComponent("reader-imports"))
        let vault = try ContentVault(root: f.root.appendingPathComponent("vault"))
        let runner = ReaderImportRunner(library: library, storage: storage, importer: ContentImporter(vault: vault, library: library))
        let resumedSource = ImportReadFixture(bytes: f.bytes)
        _ = try await runner.download(f.job.id, device: f.device, inventory: f.inventory, installation: f.job.installation,
            originalFilename: "Spanish.pack") { try await resumedSource.read($0) }
        let offsets = await resumedSource.requests(); XCTAssertEqual(offsets.first, 961)
        let stored = try await vault.verifiedObject(f.job.manifest.content); XCTAssertEqual(try Data(contentsOf: stored.url), f.bytes)
    }
    func testMalformedBindingCannotAdvanceOffsetOrPublish() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let source = ImportReadFixture(bytes: f.bytes, badBinding: true)
        await rejected { _ = try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory,
            installation: f.job.installation, originalFilename: "Spanish.pack") { try await source.read($0) } }
        let job = try await f.library.readerImportJob(f.job.id); XCTAssertEqual(job?.phase, .paused); XCTAssertEqual(job?.acknowledgedOffset, 0)
        let ids = try await f.library.libraryContentIDs(); XCTAssertTrue(ids.isEmpty)
    }
    func testDeletionDuringReadDoesNotWriteReturnedChunkOrReviveJob() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let source = ImportReadFixture(bytes: f.bytes, deletion: (f.library, f.job.manifest.content))
        await rejected { _ = try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory,
            installation: f.job.installation, originalFilename: "Spanish.pack") { try await source.read($0) } }
        let job = try await f.library.readerImportJob(f.job.id); XCTAssertEqual(job?.phase, .aborted); XCTAssertEqual(job?.acknowledgedOffset, 0)
        let path = f.root.appendingPathComponent("reader-imports/\(f.job.id.uuidString)/download")
        XCTAssertEqual(try Data(contentsOf: path), Data())
        let ids = try await f.library.libraryContentIDs(); XCTAssertTrue(ids.isEmpty)
    }
    func testForeignInstallationAndIncompleteInventorySendNoRead() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let source = ImportReadFixture(bytes: f.bytes)
        await rejected { _ = try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory,
            installation: Data(repeating: 6, count: 16), originalFilename: "Spanish.pack") { try await source.read($0) } }
        let incomplete = try ReaderInventory(reader: f.job.reader, generation: f.job.generation, contents: [f.job.manifest], complete: false)
        await rejected { _ = try await f.runner.download(f.job.id, device: f.device, inventory: incomplete,
            installation: f.job.installation, originalFilename: "Spanish.pack") { try await source.read($0) } }
        for index in [2, 18] {
            var bytes = f.device.encoded; bytes[index] = 6
            let foreign = try DeviceDescriptor(decoding: bytes)
            await rejected { _ = try await f.runner.download(f.job.id, device: foreign, inventory: f.inventory,
                installation: f.job.installation, originalFilename: "Spanish.pack") { try await source.read($0) } }
        }
        let offsets = await source.requests(); XCTAssertTrue(offsets.isEmpty)
        let saved = try await f.library.readerImportJob(f.job.id); XCTAssertEqual(saved, f.job)
    }
    func testCancellationBeforeChunkWriteRetainsZeroOffsetForRetry() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let source = ImportReadFixture(bytes: f.bytes, cancel: true)
        let task = Task {
            try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory, installation: f.job.installation,
                originalFilename: "Spanish.pack") { try await source.read($0) }
        }
        do { _ = try await task.value; XCTFail("Cancellation ignored") } catch { XCTAssertTrue(error is CancellationError) }
        let job = try await f.library.readerImportJob(f.job.id); XCTAssertEqual(job?.phase, .paused); XCTAssertEqual(job?.acknowledgedOffset, 0)
        let recovered = ImportReadFixture(bytes: f.bytes)
        _ = try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory, installation: f.job.installation,
            originalFilename: "Spanish.pack") { try await recovered.read($0) }
    }
    private func pair(_ reader: AuthenticatedImportReader) async throws -> AuthenticatedReaderSession {
        let credentials = PairingVault(storage: ImportCredentialStorage(), random: { Data(repeating: 3, count: $0) })
        return try await ReaderSession(credentials: credentials).pair(transport: reader)
    }
    func testAuthenticatedEntryChecksCapabilityAndPublishesThroughReadCommand() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes)
        let session = try await pair(reader)
        let content = try await f.runner.download(f.job.id, session: session, inventory: f.inventory, originalFilename: "Spanish.pack")
        XCTAssertEqual(content.id, f.job.manifest.content)
        let count = await reader.count(); XCTAssertGreaterThan(count, 1)
        let other = try await setup(); defer { try? FileManager.default.removeItem(at: other.root) }
        let unsupportedReader = AuthenticatedImportReader(device: other.device, bytes: other.bytes, supported: false)
        let unsupported = try await pair(unsupportedReader)
        do {
            _ = try await other.runner.download(other.job.id, session: unsupported, inventory: other.inventory, originalFilename: "Spanish.pack")
            XCTFail("Unsupported reader received export requests")
        } catch { XCTAssertEqual(error as? ReaderImportRunnerError, .unsupportedContent) }
        let sent = await unsupportedReader.count(); XCTAssertEqual(sent, 0)
        let saved = try await other.library.readerImportJob(other.job.id); XCTAssertEqual(saved, other.job)
    }
    func testAuthenticatedEntryRefusesStaleConnectionAndWrongReplyIDWithoutAdvancing() async throws {
        for stale in [true, false] {
            let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
            let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes, wrongReplyID: !stale)
            let session = try await pair(reader)
            if stale { await reader.reconnect() }
            await rejected { _ = try await f.runner.download(f.job.id, session: session, inventory: f.inventory, originalFilename: "Spanish.pack") }
            let saved = try await f.library.readerImportJob(f.job.id)
            XCTAssertEqual(saved?.phase, .paused); XCTAssertEqual(saved?.acknowledgedOffset, 0)
            let ids = try await f.library.libraryContentIDs(); XCTAssertTrue(ids.isEmpty)
        }
    }

    func testBoundFilenameMismatchIsRejectedBeforeReadingOrAdvancing() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let request = try ReaderContentMetadataRequest(generation: f.job.generation, manifest: f.job.manifest)
        let name = Data("Español.pack".utf8)
        var bytes = Data([0x4c, 0x43, 0x4e, 1, 0])
        bytes.append(request.generation); bytes.append(request.manifest.encoded)
        bytes.append(UInt8(name.count)); bytes.append(name)
        let reply = try ReaderContentMetadataReply(decoding: bytes, request: request)
        _ = try await f.library.bindReaderImportFilename(f.job.id, request: request, reply: reply)
        let source = ImportReadFixture(bytes: f.bytes)
        await rejected {
            _ = try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory,
                installation: f.job.installation, originalFilename: "changed.pack") { try await source.read($0) }
        }
        let reads = await source.requests(); XCTAssertTrue(reads.isEmpty)
        let saved = try await f.library.readerImportJob(f.job.id)
        XCTAssertEqual(saved?.phase, .queued); XCTAssertEqual(saved?.acknowledgedOffset, 0)
        let content = try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory,
            installation: f.job.installation, originalFilename: "Español.pack") { try await source.read($0) }
        XCTAssertEqual(content.originalFilename, "Español.pack")
    }

    func testAuthenticatedMetadataPreparesBoundImportWithoutCallerFilename() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes)
        let session = try await pair(reader)
        let job = try await f.runner.prepareImport(manifest: f.job.manifest, session: session, inventory: f.inventory)
        XCTAssertEqual(job.id, f.job.id)
        let name = try await f.library.readerImportFilename(job.id)
        XCTAssertEqual(name, "Spanish.pack")
        let content = try await f.runner.download(job.id, session: session, inventory: f.inventory)
        XCTAssertEqual(content.originalFilename, "Spanish.pack")
        XCTAssertEqual(content.id, job.manifest.content)
    }
    func testCancelledTaskSendsNoMetadataAndPreservesExistingImport() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes)
        let session = try await pair(reader)
        let task = Task {
            withUnsafeCurrentTask { $0?.cancel() }
            return try await f.runner.prepareImport(manifest: f.job.manifest, session: session,
                inventory: f.inventory, resuming: f.job.id)
        }
        do { _ = try await task.value; XCTFail("Cancellation ignored") }
        catch { XCTAssertTrue(error is CancellationError) }
        let requests = await reader.metadataCount(); XCTAssertEqual(requests, 0)
        let saved = try await f.library.readerImportJob(f.job.id); XCTAssertEqual(saved, f.job)
        let filename = try await f.library.readerImportFilename(f.job.id); XCTAssertNil(filename)
        let resumed = try await f.runner.prepareImport(manifest: f.job.manifest, session: session,
            inventory: f.inventory, resuming: f.job.id)
        XCTAssertEqual(resumed.id, f.job.id)
        let bound = try await f.library.readerImportFilename(f.job.id); XCTAssertEqual(bound, "Spanish.pack")
    }
    func testExistingImportMetadataResumeNeverCreatesReplacementForCancelledJob() async throws {
        for cancelled in [false, true] {
            let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
            let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes)
            let session = try await pair(reader)
            if cancelled { try await f.library.abortReaderImport(f.job.id) }
            if cancelled {
                await rejected {
                    _ = try await f.runner.prepareImport(manifest: f.job.manifest, session: session,
                        inventory: f.inventory, resuming: f.job.id)
                }
                let pending = try await f.library.pendingReaderImports(); XCTAssertTrue(pending.isEmpty)
                let saved = try await f.library.readerImportJob(f.job.id); XCTAssertEqual(saved?.phase, .aborted)
                let name = try await f.library.readerImportFilename(f.job.id); XCTAssertNil(name)
            } else {
                let resumed = try await f.runner.prepareImport(manifest: f.job.manifest, session: session,
                    inventory: f.inventory, resuming: f.job.id)
                XCTAssertEqual(resumed, f.job)
                let pending = try await f.library.pendingReaderImports(); XCTAssertEqual(pending, [f.job])
                let name = try await f.library.readerImportFilename(f.job.id); XCTAssertEqual(name, "Spanish.pack")
            }
        }
    }

    func testCancellationDuringMetadataResumeCannotBindFilenameOrCreateReplacement() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes,
            cancelOnMetadata: (f.library, f.job.id))
        let session = try await pair(reader)
        await rejected {
            _ = try await f.runner.prepareImport(manifest: f.job.manifest, session: session,
                inventory: f.inventory, resuming: f.job.id)
        }
        let pending = try await f.library.pendingReaderImports(); XCTAssertTrue(pending.isEmpty)
        let saved = try await f.library.readerImportJob(f.job.id)
        XCTAssertEqual(saved?.phase, .aborted); XCTAssertEqual(saved?.acknowledgedOffset, 0)
        let name = try await f.library.readerImportFilename(f.job.id); XCTAssertNil(name)
        let reads = await reader.count(); XCTAssertEqual(reads, 0)
    }

    func testForeignImportResumeRefusesBeforeMetadataExchange() async throws {
        for field in 0..<3 {
            let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
            try await f.library.abortReaderImport(f.job.id)
            let inventory = try ReaderInventory(reader: field == 0 ? Data(repeating: 9, count: 16) : f.inventory.reader,
                generation: field == 1 ? Data(repeating: 9, count: 16) : f.inventory.generation,
                contents: [f.job.manifest], complete: true)
            let foreign = try await f.library.enqueueReaderImport(manifest: f.job.manifest, inventory: inventory,
                installation: field == 2 ? Data(repeating: 9, count: 16) : f.job.installation)
            let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes)
            let session = try await pair(reader)
            await rejected {
                _ = try await f.runner.prepareImport(manifest: f.job.manifest, session: session,
                    inventory: f.inventory, resuming: foreign.id)
            }
            let count = await reader.metadataCount(); XCTAssertEqual(count, 0)
            let saved = try await f.library.readerImportJob(foreign.id); XCTAssertEqual(saved, foreign)
            let name = try await f.library.readerImportFilename(foreign.id); XCTAssertNil(name)
        }
    }

    func testRestartedPartialImportBindsMetadataToSameJobAndResumesAcknowledgedPrefix() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        XCTAssertGreaterThan(f.bytes.count, 961)
        try await f.library.checkpointReaderImport(f.job.id, offset: 0, phase: .downloading)
        let initial = try await f.library.readerImportJob(f.job.id)
        let downloading = try XCTUnwrap(initial)
        let offset = try await f.storage.append(Data(f.bytes.prefix(961)), to: downloading)
        try await f.library.checkpointReaderImport(f.job.id, offset: offset, phase: .downloading)
        try await f.library.checkpointReaderImport(f.job.id, offset: offset, phase: .paused)
        let reopened = try LibraryStore(url: f.root.appendingPathComponent("library.sqlite"))
        let storage = try ReaderImportStorage(root: f.root.appendingPathComponent("reader-imports"))
        let runner = ReaderImportRunner(library: reopened, storage: storage,
            importer: ContentImporter(vault: f.vault, library: reopened))
        let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes)
        let session = try await pair(reader)
        let resumed = try await runner.prepareImport(manifest: f.job.manifest, session: session,
            inventory: f.inventory, resuming: f.job.id)
        XCTAssertEqual(resumed.id, f.job.id); XCTAssertEqual(resumed.phase, .paused)
        XCTAssertEqual(resumed.acknowledgedOffset, 961)
        let content = try await runner.download(resumed.id, session: session, inventory: f.inventory)
        XCTAssertEqual(content.originalFilename, "Spanish.pack")
        let offsets = await reader.offsets(); XCTAssertEqual(offsets.first, 961)
        let saved = try await reopened.readerImportJob(f.job.id); XCTAssertEqual(saved?.phase, .completed)
        let object = try await f.vault.verifiedObject(content.id)
        XCTAssertEqual(try Data(contentsOf: object.url), f.bytes)
        let metadata = await reader.metadataCount(); XCTAssertEqual(metadata, 1)
    }

    func testBadAuthenticatedMetadataNeverBindsOrAdvancesJob() async throws {
        for stale in [true, false] {
            let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
            let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes, wrongReplyID: !stale)
            let session = try await pair(reader)
            if stale { await reader.reconnect() }
            await rejected {
                _ = try await f.runner.prepareImport(manifest: f.job.manifest, session: session, inventory: f.inventory)
            }
            let name = try await f.library.readerImportFilename(f.job.id); XCTAssertNil(name)
            let job = try await f.library.readerImportJob(f.job.id); XCTAssertEqual(job, f.job)
            let reads = await reader.count(); XCTAssertEqual(reads, 0)
        }
    }

    func testEncryptedWifiImportUsesSameDurablePublicationPath() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes)
        let session = try await pair(reader)
        let job = try await f.runner.prepareImport(manifest: f.job.manifest, session: session, inventory: f.inventory)
        let transaction = withUnsafeBytes(of: job.id.uuid) { Data($0) }
        var bytes = Data([1])
        bytes.append(job.reader); bytes.append(job.generation); bytes.append(job.installation); bytes.append(transaction)
        bytes.append(Data(repeating: 5, count: 16))
        bytes.append(Data(0..<32)); bytes.append(contentsOf: [192, 168, 4, 1])
        bytes.appendLittleEndian(8080, count: 2); bytes.appendLittleEndian(30, count: 2)
        let offer = try WifiHandoffOffer(decoding: bytes)
        let wire = try EncryptedImportReadFixture(offer: offer, bytes: f.bytes)
        let handoff = try WifiHandoffTransport(offer: offer, reader: job.reader, storageGeneration: job.generation,
            installation: job.installation, transaction: transaction, receivedAt: 0, wire: wire, now: { 0 })
        let content = try await f.runner.download(job.id, session: session, inventory: f.inventory, handoff: handoff)
        XCTAssertEqual(content.id, job.manifest.content); XCTAssertEqual(content.originalFilename, "Spanish.pack")
        let offsets = await wire.offsets(); XCTAssertEqual(offsets.first, 0); XCTAssertGreaterThan(offsets.count, 1)
        let saved = try await f.library.readerImportJob(job.id); XCTAssertEqual(saved?.phase, .completed)
        let object = try await f.vault.verifiedObject(content.id); XCTAssertEqual(object.length, UInt64(f.bytes.count))
        await handoff.close()
    }

    func testMissingImportJobClosesWifiHandoffBeforeReading() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes)
        let session = try await pair(reader)
        let job = try await f.runner.prepareImport(manifest: f.job.manifest, session: session, inventory: f.inventory)
        let transaction = withUnsafeBytes(of: job.id.uuid) { Data($0) }
        var bytes = Data([1])
        bytes.append(job.reader); bytes.append(job.generation); bytes.append(job.installation); bytes.append(transaction)
        bytes.append(Data(repeating: 5, count: 16))
        bytes.append(Data(0..<32)); bytes.append(contentsOf: [192, 168, 4, 1])
        bytes.appendLittleEndian(8080, count: 2); bytes.appendLittleEndian(30, count: 2)
        let offer = try WifiHandoffOffer(decoding: bytes)
        let wire = try EncryptedImportReadFixture(offer: offer, bytes: f.bytes)
        let handoff = try WifiHandoffTransport(offer: offer, reader: job.reader, storageGeneration: job.generation,
            installation: job.installation, transaction: transaction, receivedAt: 0, wire: wire, now: { 0 })
        do {
            _ = try await f.runner.download(UUID(), session: session, inventory: f.inventory, handoff: handoff)
            XCTFail("Missing import job accepted")
        } catch { XCTAssertEqual(error as? StoreError, .missingJob) }
        let closed = await wire.isClosed(); XCTAssertTrue(closed)
        let offsets = await wire.offsets(); XCTAssertTrue(offsets.isEmpty)
        let saved = try await f.library.readerImportJob(job.id); XCTAssertEqual(saved, job)
    }

    func testForeignWifiLeaseCannotReadOrChangeImportJob() async throws {
        for field in 0..<4 {
            let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
            let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes)
            let session = try await pair(reader)
            let job = try await f.runner.prepareImport(manifest: f.job.manifest, session: session, inventory: f.inventory)
            var identities = [job.reader, job.generation, job.installation, withUnsafeBytes(of: job.id.uuid) { Data($0) }]
            identities[field] = Data(repeating: 99, count: 16)
            var bytes = Data([1])
            for identity in identities { bytes.append(identity) }
            bytes.append(Data(repeating: 5, count: 16)); bytes.append(Data(0..<32))
            bytes.append(contentsOf: [192, 168, 4, 1])
            bytes.appendLittleEndian(8080, count: 2); bytes.appendLittleEndian(30, count: 2)
            let offer = try WifiHandoffOffer(decoding: bytes)
            let wire = try EncryptedImportReadFixture(offer: offer, bytes: f.bytes)
            let handoff = try WifiHandoffTransport(offer: offer, reader: identities[0], storageGeneration: identities[1],
                installation: identities[2], transaction: identities[3], receivedAt: 0, wire: wire, now: { 0 })
            do {
                _ = try await f.runner.download(job.id, session: session, inventory: f.inventory, handoff: handoff)
                XCTFail("Foreign Wi-Fi lease accepted")
            } catch { XCTAssertEqual(error as? ReaderImportRunnerError, .wrongHandoff) }
            let closed = await wire.isClosed(); XCTAssertTrue(closed)
            let offsets = await wire.offsets(); XCTAssertTrue(offsets.isEmpty)
            let saved = try await f.library.readerImportJob(job.id); XCTAssertEqual(saved, job)
        }
    }

    func testAuthenticatedExportAdmissionPreservesQueuedJobAndRequiresLargeRemainingContent() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes)
        let session = try await pair(reader)
        let manifest = try ContentManifest(content: ContentID(String(repeating: "12", count: 32)), kind: .epub,
            length: 2 * 1024 * 1024, formatVersion: 1, logicalIdentity: Data(count: 16))
        let inventory = try ReaderInventory(reader: f.device.identity, generation: f.device.storageGeneration,
                                            contents: [manifest, f.job.manifest], complete: true)
        let job = try await f.runner.prepareImport(manifest: manifest, session: session, inventory: inventory)
        let admitted = try await f.runner.prepareForHandoff(job.id, session: session, inventory: inventory)
        XCTAssertEqual(admitted, job)
        let saved = try await f.library.readerImportJob(job.id); XCTAssertEqual(saved, job)
        try await f.library.checkpointReaderImport(job.id, offset: 0, phase: .downloading)
        let reopened = try LibraryStore(url: f.root.appendingPathComponent("library.sqlite"))
        let restarted = ReaderImportRunner(library: reopened, storage: f.storage,
            importer: ContentImporter(vault: f.vault, library: reopened))
        let resumed = try await restarted.prepareForHandoff(job.id, session: session, inventory: inventory)
        XCTAssertEqual(resumed.phase, .downloading); XCTAssertEqual(resumed.acknowledgedOffset, 0)
        let short = try await f.runner.prepareImport(manifest: f.job.manifest, session: session, inventory: inventory)
        await rejected { _ = try await f.runner.prepareForHandoff(short.id, session: session, inventory: inventory) }
        let reads = await reader.count(); XCTAssertEqual(reads, 0)
    }

    func testLostEncryptedReplyResumesDurableOffsetOverFreshBLESessionAfterRestart() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes)
        let session = try await pair(reader)
        let job = try await f.runner.prepareImport(manifest: f.job.manifest, session: session, inventory: f.inventory)
        let transaction = withUnsafeBytes(of: job.id.uuid) { Data($0) }
        var bytes = Data([1])
        bytes.append(job.reader); bytes.append(job.generation); bytes.append(job.installation); bytes.append(transaction)
        bytes.append(Data(repeating: 5, count: 16)); bytes.append(Data(0..<32))
        bytes.append(contentsOf: [192, 168, 4, 1]); bytes.appendLittleEndian(8080, count: 2)
        bytes.appendLittleEndian(30, count: 2)
        let offer = try WifiHandoffOffer(decoding: bytes)
        let wire = try EncryptedImportReadFixture(offer: offer, bytes: f.bytes, loseReplyAt: 961)
        let handoff = try WifiHandoffTransport(offer: offer, reader: job.reader, storageGeneration: job.generation,
            installation: job.installation, transaction: transaction, receivedAt: 0, wire: wire, now: { 0 })
        await rejected { _ = try await f.runner.download(job.id, session: session, inventory: f.inventory, handoff: handoff) }
        let attempted = await wire.offsets(); XCTAssertEqual(attempted, [0, 961])
        let paused = try await f.library.readerImportJob(job.id)
        XCTAssertEqual(paused?.phase, .paused); XCTAssertEqual(paused?.acknowledgedOffset, 961)
        let active = await handoff.matches(reader: job.reader, storageGeneration: job.generation,
            installation: job.installation, transaction: transaction)
        XCTAssertFalse(active)
        let reopened = try LibraryStore(url: f.root.appendingPathComponent("library.sqlite"))
        let storage = try ReaderImportStorage(root: f.root.appendingPathComponent("reader-imports"))
        let runner = ReaderImportRunner(library: reopened, storage: storage,
            importer: ContentImporter(vault: f.vault, library: reopened))
        let fallbackReader = AuthenticatedImportReader(device: f.device, bytes: f.bytes)
        let fallback = try await pair(fallbackReader)
        let content = try await runner.download(job.id, session: fallback, inventory: f.inventory)
        let resumed = await fallbackReader.offsets(); XCTAssertEqual(resumed.first, 961)
        XCTAssertEqual(content.originalFilename, "Spanish.pack")
        let saved = try await reopened.readerImportJob(job.id); XCTAssertEqual(saved?.phase, .completed)
        let object = try await f.vault.verifiedObject(content.id)
        XCTAssertEqual(try Data(contentsOf: object.url), f.bytes)
    }

    func testDeletionDuringExportAdmissionPreventsHandoffPreparation() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let reader = AuthenticatedImportReader(device: f.device, bytes: f.bytes, deletionOnHandoff: f.library)
        let session = try await pair(reader)
        let manifest = try ContentManifest(content: ContentID(String(repeating: "12", count: 32)), kind: .epub,
            length: 2 * 1024 * 1024, formatVersion: 1, logicalIdentity: Data(count: 16))
        let inventory = try ReaderInventory(reader: f.device.identity, generation: f.device.storageGeneration,
                                            contents: [manifest], complete: true)
        let job = try await f.runner.prepareImport(manifest: manifest, session: session, inventory: inventory)
        await rejected { _ = try await f.runner.prepareForHandoff(job.id, session: session, inventory: inventory) }
        let saved = try await f.library.readerImportJob(job.id)
        XCTAssertEqual(saved?.phase, .aborted); XCTAssertEqual(saved?.acknowledgedOffset, 0)
        let content = try await f.library.content(manifest.content); XCTAssertNil(content)
        let reads = await reader.count(); XCTAssertEqual(reads, 0)
    }

    func testCancelPausedImportPersistsAbortBeforeDiscardAndAllowsNewIntent() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let source = ImportReadFixture(bytes: f.bytes, loseAt: 961)
        await rejected {
            _ = try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory,
                installation: f.job.installation, originalFilename: "Spanish.pack") { try await source.read($0) }
        }
        let staging = f.root.appendingPathComponent("reader-imports").appendingPathComponent(f.job.id.uuidString)
        XCTAssertTrue(FileManager.default.fileExists(atPath: staging.path))
        let cancelled = try await f.runner.cancel(f.job.id)
        XCTAssertEqual(cancelled.phase, .aborted); XCTAssertEqual(cancelled.acknowledgedOffset, 961)
        XCTAssertFalse(FileManager.default.fileExists(atPath: staging.path))
        let repeated = try await f.runner.cancel(f.job.id); XCTAssertEqual(repeated, cancelled)
        let reopened = try LibraryStore(url: f.root.appendingPathComponent("library.sqlite"))
        let saved = try await reopened.readerImportJob(f.job.id); XCTAssertEqual(saved, cancelled)
        let contents = try await reopened.libraryContentIDs(); XCTAssertTrue(contents.isEmpty)
        let requests = await source.requests(); XCTAssertEqual(requests, [0, 961])
        let fresh = try await reopened.enqueueReaderImport(manifest: f.job.manifest, inventory: f.inventory,
                                                          installation: f.job.installation)
        XCTAssertNotEqual(fresh.id, cancelled.id); XCTAssertEqual(fresh.phase, .queued)
    }
    func testCancelCompletedImportRefusesToRemoveVerifiedLibraryObject() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let source = ImportReadFixture(bytes: f.bytes)
        let content = try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory,
            installation: f.job.installation, originalFilename: "Spanish.pack") { try await source.read($0) }
        await rejected { _ = try await f.runner.cancel(f.job.id) }
        let saved = try await f.library.readerImportJob(f.job.id); XCTAssertEqual(saved?.phase, .completed)
        let object = try await f.vault.verifiedObject(content.id)
        XCTAssertEqual(try Data(contentsOf: object.url), f.bytes)
    }

    func testTerminalStagingCleanupIsBoundedAndPreservesPendingAndVaultObjects() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        let source = ImportReadFixture(bytes: f.bytes)
        let content = try await f.runner.download(f.job.id, device: f.device, inventory: f.inventory,
            installation: f.job.installation, originalFilename: "Spanish.pack") { try await source.read($0) }
        let terminalDirectory = f.root.appendingPathComponent("reader-imports").appendingPathComponent(f.job.id.uuidString)
        XCTAssertTrue(FileManager.default.fileExists(atPath: terminalDirectory.path))
        let pending = try await f.library.enqueueReaderImport(manifest: f.job.manifest, inventory: f.inventory,
                                                             installation: f.job.installation)
        _ = try await f.storage.prepare(pending)
        let pendingDirectory = f.root.appendingPathComponent("reader-imports").appendingPathComponent(pending.id.uuidString)
        let report = try await f.runner.cleanupFinishedImports(limit: 1)
        XCTAssertEqual(report.checkedJobs, 1); XCTAssertTrue(report.failedJobs.isEmpty)
        XCTAssertEqual(report.nextCursor, f.job.id)
        XCTAssertFalse(FileManager.default.fileExists(atPath: terminalDirectory.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: pendingDirectory.path))
        let empty = try await f.runner.cleanupFinishedImports(after: report.nextCursor, limit: 1)
        XCTAssertEqual(empty.checkedJobs, 0); XCTAssertNil(empty.nextCursor)
        let repeated = try await f.runner.cleanupFinishedImports(); XCTAssertTrue(repeated.failedJobs.isEmpty)
        let object = try await f.vault.verifiedObject(content.id)
        XCTAssertEqual(try Data(contentsOf: object.url), f.bytes)
    }
    func testTerminalCleanupReportsForeignBindingWithoutRemovingItsDirectory() async throws {
        let f = try await setup(); defer { try? FileManager.default.removeItem(at: f.root) }
        _ = try await f.storage.prepare(f.job)
        try await f.library.abortReaderImport(f.job.id)
        let directory = f.root.appendingPathComponent("reader-imports").appendingPathComponent(f.job.id.uuidString)
        let binding = directory.appendingPathComponent("binding")
        var bytes = try Data(contentsOf: binding); bytes[4] ^= 1; try bytes.write(to: binding)
        let report = try await f.runner.cleanupFinishedImports()
        XCTAssertEqual(report.failedJobs, [f.job.id])
        XCTAssertTrue(FileManager.default.fileExists(atPath: directory.path))
        let saved = try await f.library.readerImportJob(f.job.id); XCTAssertEqual(saved?.phase, .aborted)
    }

}

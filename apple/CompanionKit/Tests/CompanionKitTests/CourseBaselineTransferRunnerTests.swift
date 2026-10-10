import Foundation
import XCTest
@testable import CompanionKit

private actor BaselineTransferWire: CompanionTransport {
    let consent: CourseBaselineImportRequest
    private(set) var commands: [Command] = []
    private(set) var received = Data()
    private var state: TransferState?
    private var loseChunk: Bool
    private var loseCommit: Bool
    init(_ consent: CourseBaselineImportRequest, loseChunk: Bool = false, loseCommit: Bool = false) {
        self.consent = consent; self.loseChunk = loseChunk; self.loseCommit = loseCommit
        received.reserveCapacity(Int(consent.manifest.length))
    }
    private func update(offset: UInt64, phase: TransferPhase = .receiving) throws {
        state = try TransferState(transaction: consent.transaction, owner: consent.owner,
            storageGeneration: consent.generation, contentHash: consent.manifest.content.digest,
            length: consent.manifest.length, durableOffset: offset, phase: phase)
    }
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        commands.append(request.command)
        var result = TransferResult.ok
        switch request.command {
        case .beginCourseBaseline:
            guard try CourseBaselineImportRequest(decoding: request.payload) == consent, state == nil else {
                throw ProtocolError.value
            }
            try update(offset: 0)
        case .transferStatus:
            guard request.payload == consent.transaction else { throw ProtocolError.value }
            if state == nil { result = .noTransaction }
        case .transferChunk:
            var body = ByteReader(request.payload)
            let transaction = try body.take(16), offset = try body.number(8)
            guard transaction == consent.transaction, offset == state?.durableOffset else { throw ProtocolError.value }
            let bytes = try body.take(request.payload.count - 24)
            received.append(bytes)
            try update(offset: offset + UInt64(bytes.count))
            if loseChunk { loseChunk = false; throw URLError(.networkConnectionLost) }
        case .commit:
            guard request.payload == consent.transaction, state?.durableOffset == consent.manifest.length else {
                throw ProtocolError.value
            }
            try update(offset: consent.manifest.length, phase: .committed)
            if loseCommit { loseCommit = false; throw URLError(.networkConnectionLost) }
        default: throw ProtocolError.command
        }
        var payload = Data([result.rawValue])
        if result == .ok, let state { payload.append(state.encoded()) }
        return try ControlFrame(command: request.command, response: true, requestID: request.requestID, payload: payload)
    }
}

final class CourseBaselineTransferRunnerTests: XCTestCase {
    private struct Fixture {
        let library: LibraryStore
        let vault: ContentVault
        let database: URL
        let job: TransferJob
        let consent: CourseBaselineImportRequest
        let device: DeviceDescriptor
        let bytes: Data
    }
    private func setup(_ root: URL, supported: Bool = true) async throws -> Fixture {
        let repository = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let review = try CourseBaselineReview(decoding: Data(contentsOf:
            repository.appendingPathComponent("protocol/fixtures/CourseBaselineReview-v1.fixture")))
        let source = repository.appendingPathComponent("test/tinta/fixtures/mini.pack")
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let object = try await vault.importFile(source), metadata = try CoursePackInspector.inspect(source)
        let database = root.appendingPathComponent("library.sqlite"), library = try LibraryStore(url: database)
        try await library.putCoursePack(LibraryContent(id: object.id, kind: .course, length: object.length,
            title: "Original", originalFilename: "course.pack", languages: [metadata.locale]), metadata: metadata)
        _ = try await library.associateCourse(object.id, confirmedIdentity: review.course)
        let job = try await library.queueCourseBaselineImport(content: object.id, review: review,
                                                              installation: Data(repeating: 9, count: 16))
        let consent = try await library.courseBaselineConfirmation(job.id)!
        var bytes = Data([1, 1]); bytes.append(job.reader); bytes.append(job.storageGeneration); bytes.append(1)
        var capabilities = ReaderCapabilities([.declaredTransfers, .courseTransfers, .courseBaselineReviews])
        if supported { capabilities.insert(.courseBaselineImports) }
        bytes.appendLittleEndian(UInt64(capabilities.rawValue), count: 4)
        bytes.append(contentsOf: [80, 1, 1]); bytes.append(Data(repeating: 1, count: 32))
        return Fixture(library: library, vault: vault, database: database, job: job, consent: consent,
                       device: try DeviceDescriptor(decoding: bytes), bytes: try Data(contentsOf: source))
    }
    func testPreparedWorkKeepsBaselineRouteAcrossRestartAndCommitRecovery() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root)
        let inventory = try ReaderInventory(reader: f.job.reader, generation: f.job.storageGeneration,
            contents: [f.consent.manifest], complete: true)
        let reopened = try LibraryStore(url: f.database)
        let queued = try await reopened.prepareContentWork(reader: f.job.reader, generation: f.job.storageGeneration,
            installation: f.job.installation, inventory: inventory)
        XCTAssertEqual(queued, [.courseBaseline(f.job)])
        try await reopened.checkpoint(f.job.id, offset: 1, phase: .paused)
        let paused = try await reopened.job(f.job.id)!
        let resumed = try await reopened.prepareContentWork(reader: f.job.reader, generation: f.job.storageGeneration,
            installation: f.job.installation, inventory: inventory)
        XCTAssertEqual(resumed, [.courseBaseline(paused)])
        try await reopened.checkpoint(f.job.id, offset: f.consent.manifest.length, phase: .committing)
        let committing = try await reopened.job(f.job.id)!
        let recovery = try await reopened.prepareContentWork(reader: f.job.reader, generation: f.job.storageGeneration,
            installation: f.job.installation, inventory: inventory)
        XCTAssertEqual(recovery, [.courseBaseline(committing)])
    }
    func testPreparedBaselineWorkHonorsOwnershipStorageAndExplicitCancellation() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root)
        let inventory = try ReaderInventory(reader: f.job.reader, generation: f.job.storageGeneration,
            contents: [], complete: true)
        let foreign = try await f.library.prepareContentWork(reader: f.job.reader, generation: f.job.storageGeneration,
            installation: Data(repeating: 8, count: 16), inventory: inventory)
        XCTAssertEqual(foreign, [.inspect(f.job)])
        let replacedCard = Data(repeating: 7, count: 16)
        let replacedInventory = try ReaderInventory(reader: f.job.reader, generation: replacedCard,
            contents: [], complete: true)
        let stale = try await f.library.prepareContentWork(reader: f.job.reader, generation: replacedCard,
            installation: f.job.installation, inventory: replacedInventory)
        XCTAssertEqual(stale, [.staleGeneration(f.job)])
        _ = try await f.library.setReaderSelection(reader: f.job.reader, content: f.job.content, selected: false)
        let deselected = try await f.library.prepareContentWork(reader: f.job.reader, generation: f.job.storageGeneration,
            installation: f.job.installation, inventory: inventory)
        XCTAssertEqual(deselected, [.courseBaseline(f.job)])
        try await f.library.requestTransferAbort(f.job.id)
        let cancelled = try await f.library.prepareContentWork(reader: f.job.reader, generation: f.job.storageGeneration,
            installation: f.job.installation, inventory: inventory)
        XCTAssertEqual(cancelled, [.abort(f.job)])
    }
    func testExplicitBaselineRunUsesConsentAndSharedChunksWithoutOrdinaryBegin() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root), wire = BaselineTransferWire(f.consent)
        let completed = try await TransferRunner(library: f.library, vault: f.vault).runCourseBaseline(
            f.job.id, device: f.device, transport: wire, requireSelection: true)
        XCTAssertEqual(completed.phase, .completed)
        let commands = await wire.commands, received = await wire.received
        XCTAssertEqual(commands.first, .transferStatus)
        XCTAssertEqual(commands.filter { $0 == .beginCourseBaseline }.count, 1)
        XCTAssertFalse(commands.contains(.beginTransfer))
        XCTAssertEqual(commands.last, .commit)
        XCTAssertEqual(received, f.bytes)
    }
    func testArchivingOriginalPackPreservesActiveCourseAndDoesNotQueueInstallation() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root), wire = BaselineTransferWire(f.consent)
        let activeID = try ContentID(String(repeating: "07", count: 32))
        try await f.library.put(LibraryContent(id: activeID, kind: .course, length: 2,
            title: "Current edition", originalFilename: "current.pack"))
        _ = try await f.library.setReaderSelection(reader: f.job.reader, content: activeID, selected: true)
        let before = try await f.library.readerSelections(reader: f.job.reader)
        let review = try CourseBaselineReview(decoding: {
            var repository = URL(fileURLWithPath: #filePath)
            for _ in 0..<5 { repository.deleteLastPathComponent() }
            return try Data(contentsOf: repository.appendingPathComponent("protocol/fixtures/CourseBaselineReview-v1.fixture"))
        }())
        _ = try await f.library.queueCourseBaselineImport(content: f.job.content, review: review,
            installation: f.job.installation)
        let queuedChoices = try await f.library.readerSelections(reader: f.job.reader)
        XCTAssertEqual(queuedChoices, before)
        let active = try ContentManifest(content: activeID, kind: .course, length: 2,
            formatVersion: 1, logicalIdentity: f.consent.manifest.logicalIdentity)
        let inventory = try ReaderInventory(reader: f.job.reader, generation: f.job.storageGeneration,
            contents: [active], complete: true)
        let prepared = try await f.library.prepareContentWork(reader: f.job.reader, generation: f.job.storageGeneration,
            installation: f.job.installation, inventory: inventory)
        XCTAssertEqual(prepared, [.courseBaseline(f.job)])
        _ = try await TransferRunner(library: f.library, vault: f.vault).runCourseBaseline(
            f.job.id, device: f.device, transport: wire, requireSelection: true)
        let after = try await f.library.readerSelections(reader: f.job.reader)
        XCTAssertEqual(after, before)
        let work = try await f.library.prepareContentWork(reader: f.job.reader, generation: f.job.storageGeneration,
            installation: f.job.installation, inventory: inventory)
        XCTAssertTrue(work.isEmpty)
    }
    func testLostChunkResumesAfterRestartWithStatusAndNoSecondApproval() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root), wire = BaselineTransferWire(f.consent, loseChunk: true)
        do {
            _ = try await TransferRunner(library: f.library, vault: f.vault).runCourseBaseline(
                f.job.id, device: f.device, transport: wire)
            XCTFail("Expected lost acknowledgement")
        } catch { XCTAssertEqual((error as? URLError)?.code, .networkConnectionLost) }
        let reopened = try LibraryStore(url: f.database)
        let paused = try await reopened.job(f.job.id)
        XCTAssertEqual(paused?.phase, .paused)
        let completed = try await TransferRunner(library: reopened, vault: f.vault).runCourseBaseline(
            f.job.id, device: f.device, transport: wire, preparedTransport: true)
        XCTAssertEqual(completed.phase, .completed)
        let commands = await wire.commands, received = await wire.received
        XCTAssertEqual(commands.filter { $0 == .beginCourseBaseline }.count, 1)
        XCTAssertFalse(commands.contains(.beginTransfer))
        XCTAssertEqual(received, f.bytes)
    }
    func testLostCommitResumesFromCommittedStatusWithoutReapprovingOrReuploading() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root), wire = BaselineTransferWire(f.consent, loseCommit: true)
        do {
            _ = try await TransferRunner(library: f.library, vault: f.vault).runCourseBaseline(
                f.job.id, device: f.device, transport: wire)
            XCTFail("Expected lost commit acknowledgement")
        } catch { XCTAssertEqual((error as? URLError)?.code, .networkConnectionLost) }
        let reopened = try LibraryStore(url: f.database)
        let pending = try await reopened.job(f.job.id)
        XCTAssertEqual(pending?.phase, .committing)
        _ = try await reopened.deleteLibraryContent(f.job.content)
        let inventory = try ReaderInventory(reader: f.job.reader, generation: f.job.storageGeneration,
            contents: [], complete: true)
        let work = try await reopened.prepareContentWork(reader: f.job.reader, generation: f.job.storageGeneration,
            installation: f.job.installation, inventory: inventory)
        XCTAssertEqual(work, [.courseBaseline(pending!)])
        let before = await wire.commands
        let completed = try await TransferRunner(library: reopened, vault: f.vault).runCourseBaseline(
            f.job.id, device: f.device, transport: wire, requireSelection: true)
        XCTAssertEqual(completed.phase, .completed)
        let after = await wire.commands
        XCTAssertEqual(Array(after.dropFirst(before.count)), [.transferStatus])
    }
    func testCancelledOrDeletedArchiveNeverSendsApprovalAndRemainsAbortWork() async throws {
        for deleted in [false, true] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let f = try await setup(root), wire = BaselineTransferWire(f.consent)
            if deleted { _ = try await f.library.deleteLibraryContent(f.job.content) }
            else { try await f.library.requestTransferAbort(f.job.id) }
            do {
                _ = try await TransferRunner(library: f.library, vault: f.vault).runCourseBaseline(
                    f.job.id, device: f.device, transport: wire, requireSelection: true)
                XCTFail("Cancelled archive began upload")
            } catch { XCTAssertEqual(error as? TransferRunnerError, .abortPending) }
            let sent = await wire.commands
            XCTAssertTrue(sent.isEmpty)
            let retained = try await f.library.job(f.job.id)!
            let inventory = try ReaderInventory(reader: f.job.reader, generation: f.job.storageGeneration,
                contents: [], complete: true)
            let work = try await f.library.prepareContentWork(reader: f.job.reader, generation: f.job.storageGeneration,
                installation: f.job.installation, inventory: inventory)
            XCTAssertEqual(work, [.abort(retained)])
        }
    }
    func testMissingCapabilityAndUnpreparedHandoffSendNoApproval() async throws {
        for supported in [false, true] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let f = try await setup(root, supported: supported), wire = BaselineTransferWire(f.consent)
            do {
                _ = try await TransferRunner(library: f.library, vault: f.vault).runCourseBaseline(
                    f.job.id, device: f.device, transport: wire, preparedTransport: true)
                XCTFail("Expected refusal")
            } catch {
                if supported { XCTAssertEqual(error as? TransferCommandError, .remote(.noTransaction)) }
                else { XCTAssertEqual(error as? TransferRunnerError, .unsupportedContent) }
            }
            let commands = await wire.commands
            XCTAssertEqual(commands, supported ? [.transferStatus] : [])
        }
    }
}

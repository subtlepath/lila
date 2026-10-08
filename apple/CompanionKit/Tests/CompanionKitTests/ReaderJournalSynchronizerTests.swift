import Foundation
import XCTest
@testable import CompanionKit

private actor SyncPreparationWire: CompanionTransport {
    let readiness: JournalMergeReadiness
    let staleState: Bool
    private var operations: [String] = []
    private var active: [JournalMutation] = []
    private var candidate: [JournalMutation] = []
    private var declaration: JournalMergeDeclaration?
    private var committed: Set<Data> = []
    private var recordSize: UInt16 = 512
    init(readiness: JournalMergeReadiness = .ready, staleState: Bool = false) {
        self.readiness = readiness; self.staleState = staleState
    }
    func trace() -> [String] { operations }
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        let frontier = try TintaJournalFrontier.digest(active)
        var payload: Data
        if request.payload.count == 40 {
            operations.append("export")
            var cursor = ByteReader(Data(request.payload.suffix(4)))
            let index = Int(try cursor.number(4))
            guard index <= active.count else { throw ProtocolError.value }
            let mutation = index < active.count ? active[index] : nil
            payload = Data([1, mutation == nil ? 1 : 0, 0, 0])
            payload.appendLittleEndian(UInt64(active.count), count: 4)
            payload.appendLittleEndian(UInt64(mutation == nil ? index : index + 1), count: 4)
            payload.append(frontier)
            payload.appendLittleEndian(UInt64(mutation?.event.bytes.count ?? 0), count: 2)
            payload.appendLittleEndian(UInt64(mutation?.body.count ?? 0), count: 2)
            if let mutation { payload.append(mutation.event.bytes); payload.append(mutation.body) }
        } else if request.payload.count == JournalState.requestSize {
            operations.append("state")
            payload = Data([0x4a, 0x53, 0x53, 1])
            payload.appendLittleEndian(UInt64(active.count), count: 4)
            payload.appendLittleEndian(UInt64(recordSize), count: 2)
            payload.append(contentsOf: [0, 0])
            payload.append(staleState ? Data(repeating: 99, count: 32) : frontier)
        } else if request.payload.count == JournalMergeReadiness.requestSize {
            operations.append("readiness")
            try JournalMergeReadiness.validateRequest(request.payload, generation: Data(repeating: 2, count: 16))
            payload = Data([0x4a, 0x52, 0x52, 1, readiness.rawValue, 0, 0, 0])
        } else {
            let operation = try JournalMergeRequest.decodePayload(request.payload)
            let result: JournalMergeResult
            let count: UInt32
            switch operation {
            case .begin(let merge):
                operations.append("begin")
                if committed.contains(merge.transaction) {
                    result = .duplicate; count = UInt32(active.count)
                } else {
                    guard merge.previous.frontier == frontier, merge.previous.count == UInt32(active.count) else {
                        throw HistoryError.staleFrontier
                    }
                    if declaration != merge { declaration = merge; candidate = active }
                    result = .ok; count = UInt32(candidate.count)
                }
            case .append(let transaction, let mutation):
                operations.append("append")
                guard declaration?.transaction == transaction else { throw ProtocolError.value }
                candidate.append(mutation)
                result = .ok; count = UInt32(candidate.count)
            case .commit(let merge):
                operations.append("commit")
                guard declaration == merge, UInt32(candidate.count) == merge.merged.count,
                      try TintaJournalFrontier.digest(candidate) == merge.merged.frontier else { throw ProtocolError.value }
                active = candidate; recordSize = 1024; committed.insert(merge.transaction)
                result = .ok; count = UInt32(active.count)
            case .abort: throw ProtocolError.command
            }
            payload = Data([1, result.rawValue, 0, 0]) + operation.transaction
            payload.appendLittleEndian(UInt64(count), count: 4)
        }
        return try ControlFrame(command: .exchangeChanges, response: true, requestID: request.requestID, payload: payload)
    }
}

final class ReaderJournalSynchronizerTests: XCTestCase, @unchecked Sendable {
    private let reader = Data(repeating: 1, count: 16)
    private let generation = Data(repeating: 2, count: 16)
    private let owner = Data(repeating: 3, count: 16)
    private func inventory() throws -> ReaderInventory {
        try ReaderInventory(reader: reader, generation: generation, contents: [], complete: true)
    }
    private func prepare(_ wire: SyncPreparationWire, library: LibraryStore,
                         owner: Data? = nil) async throws -> ReaderJournalSyncPreparation {
        try await ReaderJournalSynchronizer().prepare(reader: reader, generation: generation, owner: owner ?? self.owner,
            capabilities: JournalExportPage.capability | JournalMergeReadiness.capability, transport: wire, inventory: inventory(), library: library)
    }
    func testExportOnlyFirmwareImportsHistoryAndRequestsUpgradeWithoutReadinessQuery() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"), random: { Data(repeating: 7, count: $0) })
        _ = try await library.appendPreference(origin: owner,
            preference: PreferenceBody(key: .fontPointSize, value: .integer(14)))
        let wire = SyncPreparationWire()
        let result = try await ReaderJournalSynchronizer().prepare(reader: reader, generation: generation, owner: owner,
            capabilities: JournalExportPage.capability, transport: wire, inventory: inventory(), library: library)
        XCTAssertEqual(result, .upgradeRequired)
        let trace = await wire.trace()
        XCTAssertEqual(trace, ["export"])
        let pending = try await library.pendingJournalMerge(reader: reader, generation: generation)
        XCTAssertNil(pending)
    }
    func testCoordinatorAndRunnerUploadOnceThenProduceNoFurtherChanges() async throws {
        try await verifyUploadOnce(readiness: .ready)
    }
    func testNoCourseCoordinatorAndRunnerUploadOnceThenReachVerifiedCheckpoint() async throws {
        try await verifyUploadOnce(readiness: .noCourse)
    }
    private func verifyUploadOnce(readiness: JournalMergeReadiness) async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"), random: { Data(repeating: 7, count: $0) })
        let mutation = try await library.appendPreference(origin: owner,
            preference: PreferenceBody(key: .fontPointSize, value: .integer(14)))
        let wire = SyncPreparationWire(readiness: readiness)
        guard case .upload(let job) = try await prepare(wire, library: library) else { return XCTFail("expected an upload") }
        let completed = try await JournalMergeRunner().run(transaction: job.id, reader: reader, generation: generation,
            owner: owner, transport: wire, library: library)
        XCTAssertEqual(completed.phase, .completed)
        let repeated = try await prepare(wire, library: library)
        guard case .upToDate(let checkpoint) = repeated else { return XCTFail("expected verified completion") }
        XCTAssertEqual(checkpoint.snapshot, completed.declaration.merged)
        XCTAssertEqual(checkpoint.mutations, [mutation])
        let retained = try await library.syncEvents()
        XCTAssertEqual(retained, [mutation.event])
        let pending = try await library.pendingJournalMerge(reader: reader, generation: generation)
        XCTAssertNil(pending)
        let trace = await wire.trace()
        XCTAssertEqual(trace.filter { $0 == "begin" }.count, 1)
        XCTAssertEqual(trace.filter { $0 == "append" }.count, 1)
        XCTAssertEqual(trace.filter { $0 == "commit" }.count, 1)
    }
    func testVerifiedReadyQueuesExactLocalHistory() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"), random: { Data(repeating: 7, count: $0) })
        let mutation = try await library.appendPreference(origin: owner,
            preference: PreferenceBody(key: .fontPointSize, value: .integer(14)))
        let wire = SyncPreparationWire()
        guard case .upload(let job) = try await prepare(wire, library: library) else { return XCTFail("expected an upload") }
        let mutations = try await library.journalMergeMutations(job)
        XCTAssertEqual(mutations, [mutation])
        XCTAssertEqual(job.reader, reader)
        XCTAssertEqual(job.declaration.generation, generation)
        XCTAssertEqual(job.declaration.owner, owner)
        let trace = await wire.trace()
        XCTAssertEqual(trace, ["export", "state", "readiness"])
    }
    func testNoCourseQueuesPortablePreferenceWithoutLearnerHistory() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"), random: { Data(repeating: 7, count: $0) })
        let mutation = try await library.appendPreference(origin: owner,
            preference: PreferenceBody(key: .fontPointSize, value: .integer(14)))
        let wire = SyncPreparationWire(readiness: .noCourse)
        guard case .upload(let job) = try await prepare(wire, library: library) else {
            return XCTFail("expected a generic upload without an installed course")
        }
        let mutations = try await library.journalMergeMutations(job)
        XCTAssertEqual(mutations, [mutation])
        let trace = await wire.trace()
        XCTAssertEqual(trace, ["export", "state", "readiness"])
    }
    func testLegacyAndUnavailableNeverPublishAnUpload() async throws {
        for status in [JournalMergeReadiness.migrationRequired, .unavailable] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"), random: { Data(repeating: 7, count: $0) })
            _ = try await library.appendPreference(origin: owner,
                preference: PreferenceBody(key: .fontPointSize, value: .integer(14)))
            let prepared = try await prepare(SyncPreparationWire(readiness: status), library: library)
            XCTAssertEqual(prepared, .blocked(status))
            let pending = try await library.pendingJournalMerge(reader: reader, generation: generation)
            XCTAssertNil(pending)
        }
    }
    func testStaleSnapshotRefusesReadinessAndQueuePublication() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"), random: { Data(repeating: 7, count: $0) })
        let wire = SyncPreparationWire(staleState: true)
        do {
            _ = try await prepare(wire, library: library)
            XCTFail("snapshot differs from exported history")
        } catch { XCTAssertEqual(error as? ReaderJournalSynchronizerError, .invalidSnapshot) }
        let trace = await wire.trace()
        XCTAssertEqual(trace, ["export", "state"])
        let pending = try await library.pendingJournalMerge(reader: reader, generation: generation)
        XCTAssertNil(pending)
    }
    func testPendingCandidateIsResumedBeforeExportAndWrongOwnerIsRefused() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let database = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: database, random: { Data(repeating: 7, count: $0) })
        _ = try await library.appendPreference(origin: owner,
            preference: PreferenceBody(key: .fontPointSize, value: .integer(14)))
        let first = try await prepare(SyncPreparationWire(), library: library)
        let reopened = try LibraryStore(url: database, random: { Data(repeating: 7, count: $0) }), wire = SyncPreparationWire(readiness: .unavailable)
        let resumed = try await prepare(wire, library: reopened)
        XCTAssertEqual(resumed, first)
        let trace = await wire.trace()
        XCTAssertTrue(trace.isEmpty)
        do {
            _ = try await prepare(wire, library: reopened, owner: Data(repeating: 99, count: 16))
            XCTFail("foreign owner must not resume a saved transaction")
        } catch { XCTAssertEqual(error as? ReaderSessionError, .wrongReader) }
        let unchanged = await wire.trace()
        XCTAssertTrue(unchanged.isEmpty)
    }
    func testPendingCancellationReturnsRecoveryStateBeforeAnyNetworkRequest() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"), random: { Data(repeating: 7, count: $0) })
        _ = try await library.appendPreference(origin: owner,
            preference: PreferenceBody(key: .fontPointSize, value: .integer(14)))
        guard case .upload(let job) = try await prepare(SyncPreparationWire(), library: library) else { return XCTFail("expected a job") }
        let requested = try await library.requestJournalMergeAbort(job)
        let wire = SyncPreparationWire()
        let prepared = try await prepare(wire, library: library)
        XCTAssertEqual(prepared, .cancellationPending(requested))
        let trace = await wire.trace()
        XCTAssertTrue(trace.isEmpty)
    }
    func testVerifiedEmptyHistoryIsUpToDate() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"), random: { Data(repeating: 7, count: $0) })
        let result = try await prepare(SyncPreparationWire(), library: library)
        guard case .upToDate(let checkpoint) = result else { return XCTFail("expected verified completion") }
        XCTAssertTrue(checkpoint.mutations.isEmpty)
        XCTAssertEqual(checkpoint.reader, reader)
        XCTAssertEqual(checkpoint.generation, generation)
    }
}

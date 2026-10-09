import Foundation
import XCTest
import CSQLite
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

private enum MigrationWireError: Error { case disconnected, invalidOperation }
private actor MigrationWire: CompanionTransport {
    let admission: TintaMigrationAdmission
    let incoming: [JournalMutation]
    let fault: String
    private var failed = false
    private var admitted = false
    private var started = false
    private var committed = false
    private var count: UInt32
    private var operations: [String] = []
    init(_ admission: TintaMigrationAdmission, incoming: [JournalMutation], fault: String, requiresAdmission: Bool = true) {
        self.admission = admission; self.incoming = incoming; self.fault = fault
        admitted = !requiresAdmission
        count = admission.merge.previous.count
    }
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        let operation: String
        let result: JournalMergeResult
        if request.payload.prefix(3) == Data([0x54, 0x4d, 0x41]) {
            guard !started, try TintaMigrationAdmission(decoding: request.payload) == admission else {
                throw MigrationWireError.invalidOperation
            }
            admitted = true; operation = "admission"; result = .ok
        } else {
            let merge = try JournalMergeRequest.decodePayload(request.payload)
            guard admitted, merge.transaction == admission.merge.transaction else { throw MigrationWireError.invalidOperation }
            switch merge {
            case .begin(let value):
                guard value == admission.merge else { throw MigrationWireError.invalidOperation }
                started = true; operation = "begin"; result = committed ? .duplicate : .ok
            case .append(_, let mutation):
                guard started, !committed else { throw MigrationWireError.invalidOperation }
                let index = Int(count - admission.merge.previous.count)
                guard index < incoming.count, incoming[index] == mutation else { throw MigrationWireError.invalidOperation }
                count += 1; operation = "append"; result = .ok
            case .commit(let value):
                guard started, value == admission.merge, count == admission.merge.merged.count else {
                    throw MigrationWireError.invalidOperation
                }
                operation = "commit"
                result = fault == "rejectCommit" ? .conflict : committed ? .duplicate : .ok
                if fault != "rejectCommit" { committed = true }
            case .abort(let value):
                guard value == admission.merge, !committed else { throw MigrationWireError.invalidOperation }
                operation = "abort"; result = .ok; started = false; count = admission.merge.previous.count
            }
        }
        operations.append(operation)
        if operation == fault && !failed { failed = true; throw MigrationWireError.disconnected }
        var payload = Data([1, result.rawValue, 0, 0]) + admission.merge.transaction
        payload.appendLittleEndian(UInt64(operation == "admission" ? admission.merge.previous.count : count), count: 4)
        return try ControlFrame(command: .exchangeChanges, response: true, requestID: request.requestID, payload: payload)
    }
    func trace() -> [String] { operations }
}

private actor MigrationAbortWire: CompanionTransport {
    let admission: TintaMigrationAdmission
    private var fail = true
    private var calls = 0
    private let result: JournalMergeResult
    init(_ admission: TintaMigrationAdmission, result: JournalMergeResult = .ok, initialFailure: Bool = true) {
        self.admission = admission; self.result = result; fail = initialFailure
    }
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        guard case let .abort(merge) = try JournalMergeRequest.decodePayload(request.payload), merge == admission.merge else {
            throw MigrationWireError.invalidOperation
        }
        calls += 1
        if fail { fail = false; throw MigrationWireError.disconnected }
        let payload = Data([1, result.rawValue, 0, 0]) + admission.merge.transaction + Data(count: 4)
        return try ControlFrame(command: .exchangeChanges, response: true, requestID: request.requestID, payload: payload)
    }
    func count() -> Int { calls }
}

final class TintaMigrationRunnerTests: XCTestCase, @unchecked Sendable {
    private struct Fixture {
        let root: URL
        let database: URL
        let library: LibraryStore
        let vault: ContentVault
        let backup: ContentID
        let inventory: ReaderInventory
        let admission: TintaMigrationAdmission
        let incoming: [JournalMutation]
    }
    private func fixture(learning: Bool = true) async throws -> Fixture {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let database = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: database, random: { Data(repeating: 7, count: $0) })
        var repository = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { repository.deleteLastPathComponent() }
        let imported = try await ContentImporter(vault: vault, library: library).importCoursePack(
            repository.appendingPathComponent("test/tinta/fixtures/mini.pack"))
        let course = Data(repeating: 7, count: 16)
        _ = try await library.associateCourse(imported.content.id, confirmedIdentity: course)
        var profile = legacyProfileFixture()
        if learning { profile[30] = 1; profile[32] = 1 }
        var crc = Data(); crc.appendLittleEndian(UInt64(legacyCRC32(Data(profile.prefix(39)))), count: 4)
        profile.replaceSubrange(39..<43, with: crc)
        var header = Data("TIS1".utf8)
        header.appendLittleEndian(1, count: 2); header.appendLittleEndian(80, count: 2)
        header.appendLittleEndian(1, count: 4); header.appendLittleEndian(0, count: 4)
        header.appendLittleEndian(0, count: 4); header.append(Data(count: 56))
        header.appendLittleEndian(UInt64(legacyCRC32(header)), count: 4)
        var items = Data(count: 1024); items.replaceSubrange(0..<80, with: header)
        var mark = Data(); mark.appendLittleEndian(2, count: 4); mark.append(contentsOf: [1, 0])
        mark.appendLittleEndian(UInt64(legacyCRC32(mark) & 0xffff), count: 2)
        let values: [LegacyBackupRole: Data] = [.reviews: Data(), .items: items, .profile: profile,
                                               .starred: Data("TMK1".utf8) + (learning ? mark : Data())]
        var sources: [LegacyBackupRole: URL] = [:], files: [LegacyBackupFile] = []
        files.reserveCapacity(values.count)
        for (role, bytes) in values {
            let url = root.appendingPathComponent(role.rawValue); try bytes.write(to: url); sources[role] = url
            let object = try await vault.importFile(url)
            files.append(LegacyBackupFile(role: role, id: object.id, length: object.length))
        }
        let manifest = try LegacyBackupManifest(reader: Data(repeating: 1, count: 16), generation: Data(repeating: 2, count: 16),
            course: course, files: files)
        let exportTransaction = try await library.beginLegacyBackupExport(reader: manifest.reader, generation: manifest.generation,
            course: manifest.course, previousTransaction: Data(repeating: 8, count: 16))
        let backup = try await library.preserveLegacyBackupExport(
            LegacyReaderBackupManifest(transaction: exportTransaction, manifest: manifest), sources: sources, vault: vault)
        _ = try await library.saveLegacyMigrationDraft(backup: backup, course: imported.content.id,
            confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault)
        let plan = try await library.prepareLegacyMigration(backup: backup, origin: Data(repeating: 4, count: 16), vault: vault)
        _ = try await library.installLegacyMigration(plan, backup: backup, vault: vault)
        let incoming: [JournalMutation]
        if learning { incoming = plan.mutations }
        else {
            XCTAssertTrue(plan.mutations.isEmpty)
            incoming = try await library.importLegacyPreferences(backup: backup, origin: Data(repeating: 6, count: 16),
                expected: LegacyProfile(bytes: profile).portablePreferences(), vault: vault)
        }
        let frontier = try TintaJournalFrontier.digest([])
        _ = try await library.importReaderJournal([], reader: manifest.reader, generation: manifest.generation, frontier: frontier, count: 0)
        let merge = try JournalMergeDeclaration(generation: manifest.generation, transaction: Data(repeating: 5, count: 16),
            owner: Data(repeating: 6, count: 16), previous: JournalMergeSnapshot(count: 0, recordSize: 512, frontier: frontier),
            merged: JournalMergeSnapshot(count: UInt32(incoming.count), recordSize: 1024,
                                         frontier: TintaJournalFrontier.digest(incoming)))
        let backupTransaction = Data(repeating: 8, count: 16)
        let wire = try LegacyReaderBackupManifest(transaction: backupTransaction, manifest: manifest)
        let admission = try TintaMigrationAdmission(merge: merge, course: course, resource: imported.content.id.digest,
            backupTransaction: backupTransaction, reader: manifest.reader, backupManifest: Data(SHA256.hash(data: wire.bytes)))
        let pack = try await library.courseManifest(imported.content.id)
        let inventory = try ReaderInventory(reader: manifest.reader, generation: manifest.generation, contents: [pack], complete: true)
        return Fixture(root: root, database: database, library: library, vault: vault, backup: backup,
                       inventory: inventory, admission: admission, incoming: incoming)
    }
    func testOrdinaryPlannerRequiresSelectedDictionaryInstallation() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let selection = try ContentSelection(hash: Data(repeating: 90, count: 32), name: "dictionary")
        _ = try await fixture.library.appendPreference(origin: Data(repeating: 31, count: 16),
            preference: PreferenceBody(key: .dictionary, value: .content(selection)))
        do {
            _ = try await fixture.library.prepareReaderJournalMerge(owner: fixture.admission.merge.owner,
                previous: [], snapshot: fixture.admission.merge.previous, inventory: fixture.inventory)
            XCTFail("dependent preferences must wait for content installation")
        } catch { XCTAssertEqual(error as? JournalMergePlanningError, .missingContent(selection.hash)) }
        let pending = try await fixture.library.pendingJournalMerge(reader: fixture.inventory.reader,
            generation: fixture.inventory.generation)
        XCTAssertNil(pending)
    }
    func testOrdinaryPlannerRetainsDependenciesAndBecomesNoOpAfterCompletion() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let origin = Data(repeating: 31, count: 16)
        let first = try await fixture.library.appendPreference(origin: origin,
            preference: PreferenceBody(key: .fontPointSize, value: .integer(12)))
        let second = try await fixture.library.appendPreference(origin: origin,
            preference: PreferenceBody(key: .fontPointSize, value: .integer(14)),
            ancestors: [fixture.incoming.last!.event.identity])
        let snapshot = try JournalMergeSnapshot(count: 1, recordSize: 1024, frontier: TintaJournalFrontier.digest([first]))
        _ = try await fixture.library.importReaderJournal([first], reader: fixture.inventory.reader,
            generation: fixture.inventory.generation, frontier: snapshot.frontier, count: snapshot.count)
        let value = try await fixture.library.prepareReaderJournalMerge(owner: fixture.admission.merge.owner,
            previous: [first], snapshot: snapshot, inventory: fixture.inventory)
        let job = try XCTUnwrap(value)
        let incoming = try await fixture.library.journalMergeMutations(job)
        XCTAssertEqual(Set(incoming.map(\.event.identity)), Set((fixture.incoming + [second]).map(\.event.identity)))
        XCTAssertFalse(incoming.contains(first))
        XCTAssertEqual(incoming.map(\.event.identity), incoming.map(\.event.identity).sorted())
        let retry = try await fixture.library.prepareReaderJournalMerge(owner: fixture.admission.merge.owner,
            previous: [first], snapshot: snapshot, inventory: fixture.inventory)
        XCTAssertEqual(retry, job)
        let transferring = try await fixture.library.updateJournalMerge(job, phase: .transferring,
            acknowledgedCount: job.declaration.merged.count)
        let committing = try await fixture.library.updateJournalMerge(transferring, phase: .committing,
            acknowledgedCount: job.declaration.merged.count)
        _ = try await fixture.library.updateJournalMerge(committing, phase: .completed,
            acknowledgedCount: job.declaration.merged.count)
        let noOp = try await fixture.library.prepareReaderJournalMerge(owner: fixture.admission.merge.owner,
            previous: [first] + incoming, snapshot: job.declaration.merged, inventory: fixture.inventory)
        XCTAssertNil(noOp)
    }
    func testOrdinaryPlannerRetainsUninstalledBooksAsExplicitAndImplicitAncestors() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let origin = Data(repeating: 31, count: 16)
        let implicit = try await fixture.library.appendReadingPosition(origin: origin,
            content: Data(repeating: 90, count: 32), anchor: ReadingAnchor(spine: 1, visibleTextOffset: 200))
        let explicit = try await fixture.library.appendReadingPosition(origin: Data(repeating: 32, count: 16),
            content: Data(repeating: 91, count: 32), anchor: ReadingAnchor(spine: 2, visibleTextOffset: 300))
        let preference = try await fixture.library.appendPreference(origin: origin,
            preference: PreferenceBody(key: .fontPointSize, value: .integer(12)), ancestors: [explicit.event.identity])
        let value = try await fixture.library.prepareReaderJournalMerge(owner: fixture.admission.merge.owner,
            previous: [], snapshot: fixture.admission.merge.previous, inventory: fixture.inventory)
        let job = try XCTUnwrap(value)
        let incoming = try await fixture.library.journalMergeMutations(job)
        XCTAssertTrue(incoming.contains(implicit))
        XCTAssertTrue(incoming.contains(explicit))
        XCTAssertTrue(incoming.contains(preference))
        XCTAssertEqual(incoming.count, fixture.incoming.count + 3)
    }
    func testOrdinaryPlannerPreservesForeignCourseDependencyInsteadOfDroppingIt() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        _ = try await fixture.library.appendPreference(origin: Data(repeating: 31, count: 16),
            preference: PreferenceBody(key: .fontPointSize, value: .integer(12)),
            ancestors: [fixture.incoming.last!.event.identity])
        let inventory = try ReaderInventory(reader: fixture.inventory.reader, generation: fixture.inventory.generation,
            contents: [], complete: true)
        do {
            _ = try await fixture.library.prepareReaderJournalMerge(owner: fixture.admission.merge.owner,
                previous: [], snapshot: fixture.admission.merge.previous, inventory: inventory)
            XCTFail("unavailable course ancestry must not be omitted")
        } catch {
            XCTAssertEqual(error as? JournalMergePlanningError, .unavailableCourse(fixture.admission.course))
        }
        let pending = try await fixture.library.pendingJournalMerge(reader: inventory.reader, generation: inventory.generation)
        XCTAssertNil(pending)
        let retained = try await fixture.library.syncEvents()
        XCTAssertEqual(retained.count, fixture.incoming.count + 1)
    }
    func testOrdinaryCancellationPersistsLostAbortReplyAndReleasesSlotAfterConfirmation() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let queued = try await fixture.library.queueJournalMerge(reader: fixture.inventory.reader,
            declaration: fixture.admission.merge, previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
        let wire = MigrationAbortWire(fixture.admission)
        let runner = JournalMergeRunner()
        do {
            _ = try await runner.abort(transaction: queued.id, reader: queued.reader, generation: queued.declaration.generation,
                owner: queued.declaration.owner, transport: wire, library: fixture.library)
            XCTFail("lost abort reply must remain pending")
        } catch { }
        let savedValue = try await fixture.library.journalMergeJob(queued.id)
        let requested = try XCTUnwrap(savedValue)
        XCTAssertEqual(requested.abortState, .requested)
        XCTAssertTrue(requested.paused)
        do {
            _ = try await runner.run(transaction: queued.id, reader: queued.reader, generation: queued.declaration.generation,
                owner: queued.declaration.owner, transport: wire, library: fixture.library)
            XCTFail("requested cancellation must not resume upload")
        } catch { XCTAssertEqual(error as? JournalMergeRunnerError, .abortRequested) }
        let reopened = try LibraryStore(url: fixture.database)
        let cancelled = try await runner.abort(transaction: queued.id, reader: queued.reader, generation: queued.declaration.generation,
            owner: queued.declaration.owner, transport: wire, library: reopened)
        XCTAssertEqual(cancelled.abortState, .completed)
        XCTAssertEqual(cancelled.phase, queued.phase)
        XCTAssertFalse(cancelled.paused)
        let pending = try await reopened.pendingJournalMerge(reader: queued.reader, generation: queued.declaration.generation)
        XCTAssertNil(pending)
        let baseline = try await reopened.readerJournalBaseline(reader: queued.reader, generation: queued.declaration.generation)
        XCTAssertEqual(baseline?.frontier, queued.declaration.previous.frontier)
        let retained = try await reopened.journalMergeMutations(cancelled)
        XCTAssertEqual(retained, fixture.incoming)
        let before = await wire.count()
        let repeated = try await runner.abort(transaction: queued.id, reader: queued.reader, generation: queued.declaration.generation,
            owner: queued.declaration.owner, transport: wire, library: reopened)
        XCTAssertEqual(repeated, cancelled)
        let after = await wire.count()
        XCTAssertEqual(after, before)
        var transaction = queued.id; transaction[0] ^= 1
        let next = try JournalMergeDeclaration(generation: queued.declaration.generation, transaction: transaction,
            owner: queued.declaration.owner, previous: queued.declaration.previous, merged: queued.declaration.merged)
        let newJob = try await reopened.queueJournalMerge(reader: queued.reader, declaration: next,
            previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
        XCTAssertNotEqual(newJob.id, cancelled.id)
    }
    func testOrdinaryCommittingCancellationChecksPublishedOutcome() async throws {
        for fault in ["rejectCommit", "commit"] {
            let fixture = try await fixture()
            defer { try? FileManager.default.removeItem(at: fixture.root) }
            let merge = fixture.admission.merge
            _ = try await fixture.library.queueJournalMerge(reader: fixture.inventory.reader,
                declaration: merge, previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
            let wire = MigrationWire(fixture.admission, incoming: fixture.incoming, fault: fault, requiresAdmission: false)
            let runner = JournalMergeRunner()
            do {
                _ = try await runner.run(transaction: merge.transaction, reader: fixture.inventory.reader,
                    generation: merge.generation, owner: merge.owner, transport: wire, library: fixture.library)
                XCTFail("commit fault must preserve recovery state")
            } catch { }
            let reopened = try LibraryStore(url: fixture.database)
            let resolved = try await runner.abort(transaction: merge.transaction, reader: fixture.inventory.reader,
                generation: merge.generation, owner: merge.owner, transport: wire, library: reopened)
            let trace = await wire.trace()
            let baseline = try await reopened.readerJournalBaseline(reader: fixture.inventory.reader, generation: merge.generation)
            if fault == "commit" {
                XCTAssertEqual(resolved.phase, .completed)
                XCTAssertEqual(resolved.abortState, .none)
                XCTAssertFalse(trace.contains("abort"))
                XCTAssertEqual(baseline?.frontier, merge.merged.frontier)
            } else {
                XCTAssertEqual(resolved.phase, .committing)
                XCTAssertEqual(resolved.abortState, .completed)
                XCTAssertTrue(trace.contains("abort"))
                XCTAssertEqual(baseline?.frontier, merge.previous.frontier)
            }
        }
    }
    func testOrdinaryCancellationConflictRetainsIntentAndOwnershipChecksPreventDispatch() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let queued = try await fixture.library.queueJournalMerge(reader: fixture.inventory.reader,
            declaration: fixture.admission.merge, previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
        let wire = MigrationAbortWire(fixture.admission, result: .conflict, initialFailure: false)
        let runner = JournalMergeRunner()
        do {
            _ = try await runner.abort(transaction: queued.id, reader: queued.reader, generation: queued.declaration.generation,
                owner: Data(repeating: 99, count: 16), transport: wire, library: fixture.library)
            XCTFail("foreign owner must not persist cancellation")
        } catch { XCTAssertEqual(error as? ReaderSessionError, .wrongReader) }
        let untouched = try await fixture.library.journalMergeJob(queued.id)
        XCTAssertEqual(untouched, queued)
        let firstCount = await wire.count()
        XCTAssertEqual(firstCount, 0)
        do {
            _ = try await runner.abort(transaction: queued.id, reader: queued.reader, generation: queued.declaration.generation,
                owner: queued.declaration.owner, transport: wire, library: fixture.library)
            XCTFail("conflict is not proof of cancellation")
        } catch { XCTAssertEqual(error as? JournalMergeRunnerError, .rejected(.conflict)) }
        let pending = try await fixture.library.pendingJournalMerge(reader: queued.reader, generation: queued.declaration.generation)
        XCTAssertEqual(pending?.abortState, .requested)
        let baseline = try await fixture.library.readerJournalBaseline(reader: queued.reader, generation: queued.declaration.generation)
        XCTAssertEqual(baseline?.frontier, queued.declaration.previous.frontier)
    }
    func testOrdinaryPublishedOutcomeCompletionRollsBackOnDatabaseFailureAndRetries() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let merge = fixture.admission.merge
        _ = try await fixture.library.queueJournalMerge(reader: fixture.inventory.reader,
            declaration: merge, previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
        let wire = MigrationWire(fixture.admission, incoming: fixture.incoming, fault: "commit", requiresAdmission: false)
        let runner = JournalMergeRunner()
        do {
            _ = try await runner.run(transaction: merge.transaction, reader: fixture.inventory.reader,
                generation: merge.generation, owner: merge.owner, transport: wire, library: fixture.library)
            XCTFail("lost commit reply must preserve committing phase")
        } catch { }
        try sql("CREATE TRIGGER fail_merge_completion BEFORE UPDATE ON journal_merge_jobs WHEN NEW.phase='completed' BEGIN SELECT RAISE(ABORT,'injected completion failure'); END", database: fixture.database)
        do {
            _ = try await runner.abort(transaction: merge.transaction, reader: fixture.inventory.reader,
                generation: merge.generation, owner: merge.owner, transport: wire, library: fixture.library)
            XCTFail("failed completion must leave cancellation recovery pending")
        } catch { }
        let requested = try await fixture.library.journalMergeJob(merge.transaction)
        XCTAssertEqual(requested?.abortState, .requested)
        XCTAssertEqual(requested?.phase, .committing)
        let oldBaseline = try await fixture.library.readerJournalBaseline(reader: fixture.inventory.reader, generation: merge.generation)
        XCTAssertEqual(oldBaseline?.frontier, merge.previous.frontier)
        try sql("DROP TRIGGER fail_merge_completion", database: fixture.database)
        let reopened = try LibraryStore(url: fixture.database)
        let completed = try await runner.abort(transaction: merge.transaction, reader: fixture.inventory.reader,
            generation: merge.generation, owner: merge.owner, transport: wire, library: reopened)
        XCTAssertEqual(completed.phase, .completed)
        XCTAssertEqual(completed.abortState, .none)
        let baseline = try await reopened.readerJournalBaseline(reader: fixture.inventory.reader, generation: merge.generation)
        XCTAssertEqual(baseline?.frontier, merge.merged.frontier)
        let trace = await wire.trace()
        XCTAssertFalse(trace.contains("abort"))
        let repeated = try await runner.abort(transaction: merge.transaction, reader: fixture.inventory.reader,
            generation: merge.generation, owner: merge.owner, transport: wire, library: reopened)
        XCTAssertEqual(repeated, completed)
        let repeatedTrace = await wire.trace()
        XCTAssertEqual(repeatedTrace, trace)
    }
    func testOrdinaryCancellationUpgradePreservesSchema37Jobs() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let queued = try await fixture.library.queueJournalMerge(reader: fixture.inventory.reader,
            declaration: fixture.admission.merge, previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
        try sql("DROP TABLE reader_import_filenames; DROP TABLE reader_import_jobs; DROP INDEX journal_merge_active_reader; ALTER TABLE journal_merge_jobs DROP COLUMN abort_state; CREATE UNIQUE INDEX journal_merge_active_reader ON journal_merge_jobs(reader,generation) WHERE phase!='completed'; PRAGMA user_version=37", database: fixture.database)
        let reopened = try LibraryStore(url: fixture.database)
        let restored = try await reopened.journalMergeJob(queued.id)
        XCTAssertEqual(restored, queued)
        let mutations = try await reopened.journalMergeMutations(queued)
        XCTAssertEqual(mutations, fixture.incoming)
    }
    func testOrdinaryCoordinatorDefersToPendingLegacyInstallationBeforeExport() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        _ = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: fixture.admission,
            previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
        let wire = MigrationWire(fixture.admission, incoming: fixture.incoming, fault: "")
        let prepared = try await ReaderJournalSynchronizer().prepare(reader: fixture.inventory.reader,
            generation: fixture.inventory.generation, owner: fixture.admission.merge.owner,
            capabilities: JournalExportPage.capability, transport: wire, inventory: fixture.inventory, library: fixture.library)
        XCTAssertEqual(prepared, .installationPending)
        let trace = await wire.trace()
        XCTAssertTrue(trace.isEmpty)
    }
    func testOrdinaryMergeRunnerResumesLostAcknowledgementsWithoutLegacyAdmission() async throws {
        for fault in ["begin", "append", "commit"] {
            let fixture = try await fixture()
            defer { try? FileManager.default.removeItem(at: fixture.root) }
            let merge = fixture.admission.merge
            _ = try await fixture.library.queueJournalMerge(reader: fixture.inventory.reader,
                declaration: merge, previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
            let wire = MigrationWire(fixture.admission, incoming: fixture.incoming, fault: fault, requiresAdmission: false)
            do {
                _ = try await JournalMergeRunner().run(transaction: merge.transaction, reader: fixture.inventory.reader,
                    generation: merge.generation, owner: Data(repeating: 99, count: 16), transport: wire, library: fixture.library)
                XCTFail("wrong owner must not dispatch")
            } catch { XCTAssertEqual(error as? ReaderSessionError, .wrongReader) }
            let untouched = await wire.trace()
            XCTAssertTrue(untouched.isEmpty)
            do {
                _ = try await JournalMergeRunner().run(transaction: merge.transaction, reader: fixture.inventory.reader,
                    generation: merge.generation, owner: merge.owner, transport: wire, library: fixture.library)
                XCTFail("lost reply must preserve an interrupted job")
            } catch { }
            let savedValue = try await fixture.library.journalMergeJob(merge.transaction)
            let saved = try XCTUnwrap(savedValue)
            XCTAssertTrue(saved.paused)
            let reopened = try LibraryStore(url: fixture.database)
            let runner = JournalMergeRunner()
            let completed = try await runner.run(transaction: merge.transaction, reader: fixture.inventory.reader,
                generation: merge.generation, owner: merge.owner, transport: wire, library: reopened)
            XCTAssertEqual(completed.phase, .completed)
            XCTAssertEqual(completed.acknowledgedCount, merge.merged.count)
            let trace = await wire.trace()
            XCTAssertFalse(trace.contains("admission"))
            let repeated = try await runner.run(transaction: merge.transaction, reader: fixture.inventory.reader,
                generation: merge.generation, owner: merge.owner, transport: wire, library: reopened)
            XCTAssertEqual(repeated, completed)
            let repeatedTrace = await wire.trace()
            XCTAssertEqual(repeatedTrace, trace)
        }
    }
    func testOrdinaryMergeUpgradesSchema36() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        try sql("DROP TABLE reader_import_filenames; DROP TABLE reader_import_jobs; DROP TABLE journal_merge_events; DROP TABLE journal_merge_jobs; PRAGMA user_version=36", database: fixture.database)
        let reopened = try LibraryStore(url: fixture.database)
        let queued = try await reopened.queueJournalMerge(reader: fixture.inventory.reader,
            declaration: fixture.admission.merge, previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
        let payloads = try await reopened.journalMergeMutations(queued)
        XCTAssertEqual(payloads, fixture.incoming)
        let baseline = try await reopened.readerJournalBaseline(reader: fixture.inventory.reader, generation: fixture.inventory.generation)
        XCTAssertEqual(baseline?.frontier, fixture.admission.merge.previous.frontier)
    }
    func testOrdinaryJournalMergeRetainsPayloadsAndBaselineAcrossRestart() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let declaration = fixture.admission.merge
        let queued = try await fixture.library.queueJournalMerge(reader: fixture.inventory.reader,
            declaration: declaration, previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
        let reopened = try LibraryStore(url: fixture.database)
        let restored = try await reopened.pendingJournalMerge(reader: fixture.inventory.reader, generation: fixture.inventory.generation)
        XCTAssertEqual(restored, queued)
        let retained = try await reopened.journalMergeMutations(queued)
        XCTAssertEqual(retained, fixture.incoming)
        let retry = try await reopened.queueJournalMerge(reader: fixture.inventory.reader,
            declaration: declaration, previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
        XCTAssertEqual(retry, queued)
        let transferring = try await reopened.updateJournalMerge(queued, phase: .transferring,
            acknowledgedCount: declaration.merged.count, paused: true)
        do {
            _ = try await fixture.library.updateJournalMerge(queued, phase: .transferring,
                acknowledgedCount: declaration.previous.count)
            XCTFail("stale caller must not overwrite durable acknowledgements")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        let beforeCommit = try await reopened.readerJournalBaseline(reader: fixture.inventory.reader, generation: fixture.inventory.generation)
        XCTAssertEqual(beforeCommit?.frontier, declaration.previous.frontier)
        let committing = try await reopened.updateJournalMerge(transferring, phase: .committing,
            acknowledgedCount: declaration.merged.count)
        let completed = try await reopened.updateJournalMerge(committing, phase: .completed,
            acknowledgedCount: declaration.merged.count)
        let baseline = try await reopened.readerJournalBaseline(reader: fixture.inventory.reader, generation: fixture.inventory.generation)
        XCTAssertEqual(baseline?.frontier, declaration.merged.frontier)
        XCTAssertEqual(baseline?.count, declaration.merged.count)
        let pending = try await reopened.pendingJournalMerge(reader: fixture.inventory.reader, generation: fixture.inventory.generation)
        XCTAssertNil(pending)
        let repeated = try await reopened.queueJournalMerge(reader: fixture.inventory.reader,
            declaration: declaration, previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
        XCTAssertEqual(repeated, completed)
    }
    func testOrdinaryJournalMergeQueueIsAtomicAndExcludesMigration() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let declaration = fixture.admission.merge
        try sql("CREATE TRIGGER fail_merge_event BEFORE INSERT ON journal_merge_events BEGIN SELECT RAISE(ABORT,'injected event failure'); END", database: fixture.database)
        do {
            _ = try await fixture.library.queueJournalMerge(reader: fixture.inventory.reader,
                declaration: declaration, previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
            XCTFail("payload failure must roll back the job")
        } catch { }
        let rolledBack = try await fixture.library.journalMergeJob(declaration.transaction)
        XCTAssertNil(rolledBack)
        try sql("DROP TRIGGER fail_merge_event", database: fixture.database)
        _ = try await fixture.library.queueJournalMerge(reader: fixture.inventory.reader,
            declaration: declaration, previous: [], incoming: fixture.incoming, inventory: fixture.inventory)
        do {
            _ = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: fixture.admission,
                previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
            XCTFail("migration cannot replace an ordinary journal candidate")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
    }
    func testUnresolvedTintaPreferencesRefusePreparationUntilExplicitChoice() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        _ = try await fixture.library.appendPreference(origin: Data(repeating: 40, count: 16),
            preference: PreferenceBody(key: .tintaNewPerDay, value: .integer(10)))
        let chosen = try PreferenceBody(key: .tintaNewPerDay, value: .integer(20))
        _ = try await fixture.library.appendPreference(origin: Data(repeating: 41, count: 16), preference: chosen)
        let saved = try await fixture.library.legacyMigrationDraft(fixture.backup)
        let draft = try XCTUnwrap(saved)
        do {
            _ = try await fixture.library.prepareReaderTintaMigration(backup: fixture.backup, content: draft.course,
                owner: fixture.admission.merge.owner, previous: [], snapshot: fixture.admission.merge.previous,
                inventory: fixture.inventory, vault: fixture.vault, expectedDraft: draft)
            XCTFail("Unresolved portable Tinta choices must not be installed")
        } catch { XCTAssertEqual(error as? StoreError, .unresolvedPreferences) }
        let states = try await fixture.library.preferences()
        let state = try XCTUnwrap(states.first { $0.key == .tintaNewPerDay })
        _ = try await fixture.library.resolvePreference(origin: Data(repeating: 42, count: 16), preference: chosen,
            expectedHeads: state.resolutionAncestors)
        let job = try await fixture.library.prepareReaderTintaMigration(backup: fixture.backup, content: draft.course,
            owner: fixture.admission.merge.owner, previous: [], snapshot: fixture.admission.merge.previous,
            inventory: fixture.inventory, vault: fixture.vault, expectedDraft: draft)
        let mutations = try await fixture.library.tintaMigrationMutations(job)
        let resolved = try PreferenceHistory.reconcile(mutations)
        XCTAssertFalse(resolved.contains { $0.requiresResolution })
        XCTAssertEqual(resolved.first?.candidates.first?.body, chosen)
    }
    func testSchema35UpgradePreservesBackupAndExistingPreferenceHistory() async throws {
        let fixture = try await fixture(learning: false)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let before = try await fixture.library.syncEvents()
        try sql("DROP TABLE reader_import_filenames; DROP TABLE reader_import_jobs; DROP TABLE journal_merge_events; DROP TABLE journal_merge_jobs; DROP TABLE legacy_preference_imports; PRAGMA user_version=35", database: fixture.database)
        let upgraded = try LibraryStore(url: fixture.database)
        let after = try await upgraded.syncEvents()
        XCTAssertEqual(after, before)
        let draft = try await upgraded.legacyMigrationDraft(fixture.backup)
        XCTAssertNotNil(draft)
        _ = try await fixture.vault.verifiedLegacyBackup(fixture.backup)
    }
    func testBackupPreferenceImportIsAtomicAndReusesVerifiedIdentitiesAcrossOwners() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let snapshot = try await fixture.vault.legacyBackupSnapshot(fixture.backup)
        let expected = try snapshot.profile.portablePreferences()
        let before = try await fixture.library.syncEvents()
        try sql("CREATE TRIGGER fail_preference_receipt BEFORE INSERT ON legacy_preference_imports BEGIN SELECT RAISE(ABORT,'injected receipt failure'); END", database: fixture.database)
        do {
            _ = try await fixture.library.importLegacyPreferences(backup: fixture.backup,
                origin: Data(repeating: 40, count: 16), expected: expected, vault: fixture.vault)
            XCTFail("Receipt failure must roll back the preference events")
        } catch { XCTAssertTrue(error is StoreError) }
        let rolledBack = try await fixture.library.syncEvents()
        XCTAssertEqual(rolledBack, before)
        try sql("DROP TRIGGER fail_preference_receipt", database: fixture.database)
        let imported = try await fixture.library.importLegacyPreferences(backup: fixture.backup,
            origin: Data(repeating: 40, count: 16), expected: expected, vault: fixture.vault)
        XCTAssertEqual(imported.count, 9)
        let reopened = try LibraryStore(url: fixture.database)
        let repeated = try await reopened.importLegacyPreferences(backup: fixture.backup,
            origin: Data(repeating: 41, count: 16), expected: expected, vault: fixture.vault)
        XCTAssertEqual(repeated, imported)
        let after = try await reopened.syncEvents()
        XCTAssertEqual(after.count, before.count + 9)
        var changed = expected
        changed[0] = try PreferenceBody(key: .tintaNewPerDay, value: .integer(20))
        do {
            _ = try await reopened.importLegacyPreferences(backup: fixture.backup,
                origin: Data(repeating: 41, count: 16), expected: changed, vault: fixture.vault)
            XCTFail("Confirmation must match the verified backup")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let unchanged = try await reopened.syncEvents()
        XCTAssertEqual(unchanged, after)
    }
    func testEmptyLearningBackupPreparesPreferenceOnlyMigrationWithoutInventingLearningEvents() async throws {
        let fixture = try await fixture(learning: false)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let saved = try await fixture.library.legacyMigrationDraft(fixture.backup)
        let draft = try XCTUnwrap(saved)
        let job = try await fixture.library.prepareReaderTintaMigration(backup: fixture.backup, content: draft.course,
            owner: fixture.admission.merge.owner, previous: [], snapshot: fixture.admission.merge.previous,
            inventory: fixture.inventory, vault: fixture.vault, expectedDraft: draft)
        let delivery = try await fixture.library.tintaMigrationMutations(job)
        XCTAssertEqual(delivery.count, 9)
        XCTAssertTrue(delivery.allSatisfy { $0.event.kind == .preference })
        XCTAssertEqual(delivery, fixture.incoming)
        let wire = MigrationWire(job.admission, incoming: delivery, fault: "none")
        let completed = try await TintaMigrationRunner().run(transaction: job.id, reader: job.admission.reader,
            generation: job.admission.merge.generation, owner: job.admission.merge.owner,
            transport: wire, library: fixture.library)
        XCTAssertEqual(completed.phase, .completed)
        let after = try await fixture.library.syncEvents()
        XCTAssertEqual(after.count, 9)
        XCTAssertTrue(after.allSatisfy { $0.kind == .preference })
        _ = try await fixture.vault.verifiedLegacyBackup(fixture.backup)
    }
    func testPreparationIncludesConfirmedPortableTintaPreferences() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let preference = try PreferenceBody(key: .tintaNewPerDay, value: .integer(20))
        let mutation = try await fixture.library.appendPreference(origin: Data(repeating: 31, count: 16), preference: preference)
        let saved = try await fixture.library.legacyMigrationDraft(fixture.backup)
        let draft = try XCTUnwrap(saved)
        let job = try await fixture.library.prepareReaderTintaMigration(backup: fixture.backup, content: draft.course,
            owner: fixture.admission.merge.owner, previous: [], snapshot: fixture.admission.merge.previous,
            inventory: fixture.inventory, vault: fixture.vault, expectedDraft: draft)
        let delivery = try await fixture.library.tintaMigrationMutations(job)
        XCTAssertTrue(delivery.contains(mutation))
        XCTAssertEqual(delivery.count, fixture.incoming.count + 1)
        XCTAssertEqual(try PreferenceBody(decoding: mutation.body), preference)
    }
    func testCommittingAbortChecksPublishedOutcomeAndNeverRollsBackCompletedMerge() async throws {
        for fault in ["rejectCommit", "commit"] {
            let fixture = try await fixture()
            defer { try? FileManager.default.removeItem(at: fixture.root) }
            let admission = fixture.admission
            let job = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: admission,
                previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
            let wire = MigrationWire(admission, incoming: fixture.incoming, fault: fault)
            do {
                _ = try await TintaMigrationRunner().run(transaction: job.id, reader: admission.reader,
                    generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: fixture.library)
                XCTFail("Rejected/lost Commit must retain the committing phase")
            } catch {}
            let reopened = try LibraryStore(url: fixture.database)
            let saved = try await reopened.tintaMigrationJob(job.id)
            XCTAssertEqual(saved?.phase, .committing)
            let result = try await TintaMigrationRunner().abort(transaction: job.id, reader: admission.reader,
                generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: reopened)
            let trace = await wire.trace()
            let baseline = try await reopened.readerJournalBaseline(reader: admission.reader, generation: admission.merge.generation)
            if fault == "commit" {
                XCTAssertEqual(result.phase, .completed)
                XCTAssertEqual(result.abortState, .none)
                XCTAssertFalse(trace.contains("abort"))
                XCTAssertEqual(baseline?.frontier, admission.merge.merged.frontier)
            } else {
                XCTAssertEqual(result.phase, .committing)
                XCTAssertEqual(result.abortState, .completed)
                XCTAssertEqual(trace.last, "abort")
                XCTAssertEqual(baseline?.frontier, admission.merge.previous.frontier)
            }
        }
    }
    func testLostAbortAcknowledgementRetriesFrozenDeclarationAfterRestart() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let admission = fixture.admission
        let job = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: admission,
            previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
        let wire = MigrationAbortWire(admission)
        do {
            _ = try await TintaMigrationRunner().abort(transaction: job.id, reader: admission.reader,
                generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: fixture.library)
            XCTFail("Lost abort reply must keep abort pending")
        } catch { XCTAssertTrue(error is MigrationWireError) }
        let reopened = try LibraryStore(url: fixture.database)
        let saved = try await reopened.tintaMigrationJob(job.id)
        XCTAssertEqual(saved?.abortState, .requested)
        let terminal = try await TintaMigrationRunner().abort(transaction: job.id, reader: admission.reader,
            generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: reopened)
        XCTAssertEqual(terminal.abortState, .completed)
        let repeated = try await TintaMigrationRunner().abort(transaction: job.id, reader: admission.reader,
            generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: reopened)
        XCTAssertEqual(repeated, terminal)
        let count = await wire.count(); XCTAssertEqual(count, 2)
    }
    func testAbortIntentSurvivesRestartAndRetiresOnlyAfterAcknowledgement() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let admission = fixture.admission
        let job = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: admission,
            previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
        let requested = try await fixture.library.requestTintaMigrationAbort(job)
        XCTAssertEqual(requested.abortState, .requested)
        XCTAssertTrue(requested.paused)
        let reopened = try LibraryStore(url: fixture.database)
        let pending = try await reopened.pendingTintaMigration(reader: admission.reader, generation: admission.merge.generation)
        XCTAssertEqual(pending, requested)
        let wire = MigrationWire(admission, incoming: fixture.incoming, fault: "none")
        do {
            _ = try await TintaMigrationRunner().run(transaction: job.id, reader: admission.reader,
                generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: reopened)
            XCTFail("Abort-requested jobs must not resume installation")
        } catch { XCTAssertEqual(error as? TintaMigrationRunnerError, .abortRequested) }
        let trace = await wire.trace(); XCTAssertTrue(trace.isEmpty)
        do {
            _ = try await reopened.completeTintaMigrationAbort(job)
            XCTFail("Stale state cannot retire an abort")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        let terminal = try await reopened.completeTintaMigrationAbort(requested)
        XCTAssertEqual(terminal.abortState, .completed)
        XCTAssertEqual(terminal.phase, job.phase)
        XCTAssertFalse(terminal.paused)
        let absent = try await reopened.pendingTintaMigration(reader: admission.reader, generation: admission.merge.generation)
        XCTAssertNil(absent)
        let baseline = try await reopened.readerJournalBaseline(reader: admission.reader, generation: admission.merge.generation)
        XCTAssertEqual(baseline?.frontier, admission.merge.previous.frontier)
        XCTAssertEqual(baseline?.count, admission.merge.previous.count)
        let retained = try await reopened.tintaMigrationMutations(terminal)
        XCTAssertEqual(retained, fixture.incoming)
        let repeated = try await reopened.completeTintaMigrationAbort(terminal)
        XCTAssertEqual(repeated, terminal)
        let replacementMerge = try JournalMergeDeclaration(generation: admission.merge.generation,
            transaction: Data(repeating: 30, count: 16), owner: admission.merge.owner,
            previous: admission.merge.previous, merged: admission.merge.merged)
        let replacementAdmission = try TintaMigrationAdmission(merge: replacementMerge, course: admission.course,
            resource: admission.resource, backupTransaction: admission.backupTransaction,
            reader: admission.reader, backupManifest: admission.backupManifest)
        var replacement = try await reopened.queueTintaMigration(backup: fixture.backup, admission: replacementAdmission,
            previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
        replacement = try await reopened.updateTintaMigration(replacement, phase: .admitting, acknowledgedCount: 0)
        replacement = try await reopened.updateTintaMigration(replacement, phase: .transferring, acknowledgedCount: 0)
        replacement = try await reopened.updateTintaMigration(replacement, phase: .committing,
            acknowledgedCount: replacement.admission.merge.merged.count)
        let requestedCommit = try await reopened.requestTintaMigrationAbort(replacement)
        XCTAssertEqual(requestedCommit.phase, .committing)
        XCTAssertEqual(requestedCommit.abortState, .requested)
    }
    func testSchema34UpgradePreservesFrozenMigrationAndDefaultsAbortState() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let job = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: fixture.admission,
            previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
        try sql("DROP TABLE reader_import_filenames; DROP TABLE reader_import_jobs; DROP TABLE journal_merge_events; DROP TABLE journal_merge_jobs; DROP TABLE legacy_preference_imports; DROP INDEX tinta_migration_active_reader; ALTER TABLE tinta_migration_jobs DROP COLUMN abort_state; CREATE UNIQUE INDEX tinta_migration_active_reader ON tinta_migration_jobs(reader,generation) WHERE phase!='completed'; PRAGMA user_version=34", database: fixture.database)
        let upgraded = try LibraryStore(url: fixture.database)
        let restored = try await upgraded.tintaMigrationJob(job.id)
        XCTAssertEqual(restored, job)
        let mutations = try await upgraded.tintaMigrationMutations(job)
        XCTAssertEqual(mutations, fixture.incoming)
    }
    func testMigrationRetainsCompatibleSourcePackHashes() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let resource = try ContentID(String(repeating: "a", count: 64))
        try await fixture.library.acceptCloudCourseAssociation(
            CloudCourseAssociation(content: resource, identity: fixture.admission.course))
        let body = try TintaBody(subject: TintaSubject(course: fixture.admission.course, uid: 1), value: .star(true)).encoded
        let event = try SyncEvent(identity: EventIdentity(origin: Data(repeating: 20, count: 16), epoch: 1, sequence: 1),
            storageGeneration: Data(repeating: 21, count: 16), kind: .star,
            resource: resource.digest, bodyHash: Data(SHA256.hash(data: body)))
        let mutation = try JournalMutation(event: event, body: body)
        _ = try await fixture.library.importTintaEvents([mutation])
        let incoming = fixture.incoming + [mutation]
        let old = fixture.admission
        let merge = try JournalMergeDeclaration(generation: old.merge.generation, transaction: old.merge.transaction,
            owner: old.merge.owner, previous: old.merge.previous,
            merged: JournalMergeSnapshot(count: UInt32(incoming.count), recordSize: 1024,
                frontier: TintaJournalFrontier.digest(incoming)))
        let admission = try TintaMigrationAdmission(merge: merge, course: old.course, resource: old.resource,
            backupTransaction: old.backupTransaction, reader: old.reader, backupManifest: old.backupManifest)
        let job = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: admission,
            previous: [], incoming: incoming, inventory: fixture.inventory, vault: fixture.vault)
        let saved = try await fixture.library.tintaMigrationMutations(job)
        XCTAssertEqual(saved.last, mutation)
        XCTAssertNotEqual(saved.last?.event.resource, admission.resource)
        do {
            try await fixture.library.acceptCloudCourseAssociation(
                CloudCourseAssociation(content: resource, identity: Data(repeating: 22, count: 16)))
            XCTFail("Source hash must retain its logical course association")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
    }
    func testLostHandoffPreparationRepliesResumeAfterReopening() async throws {
        for fault in ["admission", "begin"] {
            let fixture = try await fixture()
            defer { try? FileManager.default.removeItem(at: fixture.root) }
            let admission = fixture.admission
            let job = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: admission,
                previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
            let wire = MigrationWire(admission, incoming: fixture.incoming, fault: fault)
            do {
                _ = try await TintaMigrationRunner().prepareForHandoff(transaction: job.id, reader: admission.reader,
                    generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: fixture.library)
                XCTFail("Lost preparation reply must pause the saved phase")
            } catch { XCTAssertTrue(error is MigrationWireError) }
            let reopened = try LibraryStore(url: fixture.database)
            let saved = try await reopened.tintaMigrationJob(job.id)
            XCTAssertEqual(saved?.paused, true)
            XCTAssertEqual(saved?.phase, fault == "admission" ? .admitting : .transferring)
            let runner = TintaMigrationRunner()
            let prepared = try await runner.prepareForHandoff(transaction: job.id, reader: admission.reader,
                generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: reopened)
            XCTAssertEqual(prepared.phase, .transferring)
            XCTAssertFalse(prepared.paused)
            let completed = try await runner.run(transaction: job.id, reader: admission.reader,
                generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: reopened)
            XCTAssertEqual(completed.phase, .completed)
            let trace = await wire.trace()
            XCTAssertEqual(trace.filter { $0 == "admission" }.count, fault == "admission" ? 2 : 1)
            XCTAssertEqual(trace.filter { $0 == "append" }.count, fixture.incoming.count)
        }
    }
    func testHandoffPreparationStopsBeforeAppendAndResumesSameCandidate() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let admission = fixture.admission
        let job = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: admission,
            previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
        let wire = MigrationWire(admission, incoming: fixture.incoming, fault: "none")
        let runner = TintaMigrationRunner()
        let prepared = try await runner.prepareForHandoff(transaction: job.id, reader: admission.reader,
            generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: fixture.library)
        XCTAssertEqual(prepared.phase, .transferring)
        XCTAssertEqual(prepared.acknowledgedCount, admission.merge.previous.count)
        let before = await wire.trace()
        XCTAssertEqual(before, ["admission", "begin"])
        let completed = try await runner.run(transaction: job.id, reader: admission.reader,
            generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: fixture.library)
        XCTAssertEqual(completed.phase, .completed)
        let after = await wire.trace()
        XCTAssertEqual(after.filter { $0 == "admission" }.count, 1)
        XCTAssertEqual(after.filter { $0 == "append" }.count, fixture.incoming.count)
    }
    func testConfirmedSharedBackupPreparationReusesCanonicalEvents() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let original = try await fixture.vault.verifiedLegacyBackup(fixture.backup)
        let manifest = try LegacyBackupManifest(reader: Data(repeating: 9, count: 16),
            generation: Data(repeating: 10, count: 16), course: original.course, files: original.files)
        let transaction = try await fixture.library.beginLegacyBackupExport(reader: manifest.reader,
            generation: manifest.generation, course: manifest.course, previousTransaction: Data(repeating: 11, count: 16))
        var sources: [LegacyBackupRole: URL] = [:]
        for file in manifest.files { sources[file.role] = fixture.root.appendingPathComponent(file.role.rawValue) }
        let backup = try await fixture.library.preserveLegacyBackupExport(
            LegacyReaderBackupManifest(transaction: transaction, manifest: manifest), sources: sources, vault: fixture.vault)
        _ = try await fixture.library.confirmSharedLegacyBackup(backup, canonical: fixture.backup, vault: fixture.vault)
        let snapshot = try await fixture.vault.legacyBackupSnapshot(fixture.backup)
        let expectedPreferences = try snapshot.profile.portablePreferences()
        let canonicalPreferences = try await fixture.library.importLegacyPreferences(backup: fixture.backup,
            origin: Data(repeating: 25, count: 16), expected: expectedPreferences, vault: fixture.vault)
        let beforePreferences = try await fixture.library.syncEvents()
        let sharedPreferences = try await fixture.library.importLegacyPreferences(backup: backup,
            origin: Data(repeating: 26, count: 16), expected: expectedPreferences, vault: fixture.vault)
        XCTAssertEqual(sharedPreferences, canonicalPreferences)
        let afterPreferences = try await fixture.library.syncEvents()
        XCTAssertEqual(afterPreferences, beforePreferences)
        let before = try await fixture.library.syncEvents()
        let frontier = try TintaJournalFrontier.digest([])
        _ = try await fixture.library.importReaderJournal([], reader: manifest.reader,
            generation: manifest.generation, frontier: frontier, count: 0)
        let inventory = try ReaderInventory(reader: manifest.reader, generation: manifest.generation,
            contents: fixture.inventory.contents, complete: true)
        let content = try ContentID(fixture.admission.resource.map { String(format: "%02x", $0) }.joined())
        let job = try await fixture.library.prepareReaderTintaMigration(backup: backup, content: content,
            owner: Data(repeating: 12, count: 16), previous: [],
            snapshot: JournalMergeSnapshot(count: 0, recordSize: 512, frontier: frontier),
            inventory: inventory, vault: fixture.vault)
        let after = try await fixture.library.syncEvents()
        XCTAssertEqual(after, before)
        XCTAssertEqual(job.backup, backup)
        XCTAssertEqual(job.admission.backupTransaction, transaction)
        XCTAssertEqual(job.admission.reader, manifest.reader)
        let delivery = try await fixture.library.tintaMigrationMutations(job)
        XCTAssertEqual(delivery, (fixture.incoming + canonicalPreferences).sorted { $0.event.identity < $1.event.identity })
        let independentDraft = try await fixture.library.legacyMigrationDraft(backup)
        XCTAssertNil(independentDraft)
        let independentManifest = try LegacyBackupManifest(reader: Data(repeating: 27, count: 16),
            generation: Data(repeating: 28, count: 16), course: original.course, files: original.files)
        let independentBackup = try await fixture.library.preserveLegacyBackup(independentManifest, sources: sources, vault: fixture.vault)
        _ = try await fixture.library.importLegacyPreferences(backup: independentBackup, origin: Data(repeating: 29, count: 16),
            expected: expectedPreferences, vault: fixture.vault)
        let independentEvents = try await fixture.library.syncEvents()
        do {
            _ = try await fixture.library.confirmSharedLegacyBackup(independentBackup, canonical: fixture.backup, vault: fixture.vault)
            XCTFail("Independently imported preferences cannot be silently relabelled as shared history")
        } catch { XCTAssertEqual(error as? LegacySharedHistoryError, .alreadyPrepared) }
        let retained = try await fixture.library.syncEvents()
        XCTAssertEqual(retained, independentEvents)
        let alias = try await fixture.library.sharedLegacyCanonical(independentBackup)
        XCTAssertNil(alias)
    }
    func testReviewedPreparationUsesVerifiedExportAndRejectsChangedSnapshot() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let savedDraft = try await fixture.library.legacyMigrationDraft(fixture.backup)
        let draft = try XCTUnwrap(savedDraft)
        let content = draft.course
        let snapshot = fixture.admission.merge.previous
        let changed = try JournalMergeSnapshot(count: 0, recordSize: 512, frontier: Data(repeating: 9, count: 32))
        do {
            _ = try await fixture.library.prepareReaderTintaMigration(backup: fixture.backup, content: content,
                owner: Data(repeating: 4, count: 16), previous: [], snapshot: changed,
                inventory: fixture.inventory, vault: fixture.vault, expectedDraft: draft)
            XCTFail("Changed reader snapshot must be refused")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let absent = try await fixture.library.latestTintaMigration(backup: fixture.backup,
            reader: fixture.inventory.reader, generation: fixture.inventory.generation)
        XCTAssertNil(absent)
        for owner in [Data(), Data(count: 16), Data(repeating: 4, count: 15)] {
            do {
                _ = try await fixture.library.prepareReaderTintaMigration(backup: fixture.backup, content: content,
                    owner: owner, previous: [], snapshot: snapshot, inventory: fixture.inventory,
                    vault: fixture.vault, expectedDraft: draft)
                XCTFail("Invalid owner must be refused before preparing history")
            } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        }
        let missingPack = try ReaderInventory(reader: fixture.inventory.reader,
            generation: fixture.inventory.generation, contents: [], complete: true)
        do {
            _ = try await fixture.library.prepareReaderTintaMigration(backup: fixture.backup, content: content,
                owner: Data(repeating: 4, count: 16), previous: [], snapshot: snapshot,
                inventory: missingPack, vault: fixture.vault, expectedDraft: draft)
            XCTFail("Missing installed course must be refused")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let before = try await fixture.library.syncEvents()
        let job = try await fixture.library.prepareReaderTintaMigration(backup: fixture.backup, content: content,
            owner: fixture.admission.merge.owner, previous: [], snapshot: snapshot,
            inventory: fixture.inventory, vault: fixture.vault, expectedDraft: draft)
        let after = try await fixture.library.syncEvents()
        XCTAssertEqual(after, before)
        XCTAssertEqual(job.admission.merge.owner, fixture.admission.merge.owner)
        let delivery = try await fixture.library.tintaMigrationMutations(job)
        XCTAssertEqual(delivery, fixture.incoming.sorted { $0.event.identity < $1.event.identity })
        XCTAssertNotEqual(delivery.first?.event.identity.origin, job.admission.merge.owner)
        XCTAssertEqual(job.admission.backupTransaction, fixture.admission.backupTransaction)
        XCTAssertEqual(job.admission.backupManifest, fixture.admission.backupManifest)
        XCTAssertEqual(job.admission.merge.merged, fixture.admission.merge.merged)
        let restored = try await fixture.library.latestTintaMigration(backup: fixture.backup,
            reader: fixture.inventory.reader, generation: fixture.inventory.generation)
        XCTAssertEqual(restored, job)
    }
    func testLostAcknowledgementsResumeFrozenJobsAfterReopeningWithoutRepeatingEvents() async throws {
        for fault in ["admission", "append", "commit"] {
            let fixture = try await fixture()
            defer { try? FileManager.default.removeItem(at: fixture.root) }
            XCTAssertGreaterThan(fixture.incoming.count, 1)
            let admission = fixture.admission
            let job = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: admission,
                previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
            XCTAssertEqual(job.phase, .queued)
            let wire = MigrationWire(admission, incoming: fixture.incoming, fault: fault)
            do {
                _ = try await TintaMigrationRunner().run(transaction: job.id, reader: admission.reader,
                    generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: fixture.library)
                XCTFail("Lost acknowledgement should pause the job")
            } catch { XCTAssertTrue(error is MigrationWireError) }
            let savedPaused = try await fixture.library.tintaMigrationJob(job.id)
            let paused = try XCTUnwrap(savedPaused)
            XCTAssertTrue(paused.paused)
            XCTAssertEqual(paused.phase, fault == "admission" ? .admitting : fault == "append" ? .transferring : .committing)
            let reopened = try LibraryStore(url: fixture.database)
            let completed = try await TintaMigrationRunner().run(transaction: job.id, reader: admission.reader,
                generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: reopened)
            XCTAssertEqual(completed.phase, .completed); XCTAssertFalse(completed.paused)
            XCTAssertEqual(completed.acknowledgedCount, admission.merge.merged.count)
            let trace = await wire.trace()
            XCTAssertEqual(trace.filter { $0 == "append" }.count, fixture.incoming.count)
            XCTAssertEqual(trace.filter { $0 == "admission" }.count, fault == "admission" ? 2 : 1)
            let repeated = try await reopened.queueTintaMigration(backup: fixture.backup, admission: admission,
                previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
            XCTAssertEqual(repeated, completed)
            let retry = try await TintaMigrationRunner().run(transaction: job.id, reader: admission.reader,
                generation: admission.merge.generation, owner: admission.merge.owner, transport: wire, library: reopened)
            XCTAssertEqual(retry, completed)
            let repeatedTrace = await wire.trace(); XCTAssertEqual(repeatedTrace, trace)
            let baseline = try await reopened.readerJournalBaseline(reader: admission.reader, generation: admission.merge.generation)
            XCTAssertEqual(baseline?.frontier, admission.merge.merged.frontier)
        }
    }
    private func sql(_ statement: String, database: URL) throws {
        var handle: OpaquePointer?
        guard sqlite3_open(database.path, &handle) == SQLITE_OK else { throw StoreError.database("test open") }
        defer { sqlite3_close(handle) }
        guard sqlite3_exec(handle, statement, nil, nil, nil) == SQLITE_OK else { throw StoreError.database("test SQL") }
    }
    func testQueueRollbackLeavesNoPartialJobAndAllowsRecreatedStoreRetry() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        try sql("CREATE TRIGGER reject_migration_event BEFORE INSERT ON tinta_migration_events BEGIN SELECT RAISE(ABORT,'injected'); END", database: fixture.database)
        do {
            _ = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: fixture.admission,
                previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
            XCTFail("Injected write failure accepted")
        } catch { XCTAssertTrue(error is StoreError) }
        let reopened = try LibraryStore(url: fixture.database)
        let absent = try await reopened.tintaMigrationJob(fixture.admission.merge.transaction)
        XCTAssertNil(absent)
        let retained = try await reopened.syncEvents(); XCTAssertEqual(retained.count, fixture.incoming.count)
        try sql("DROP TRIGGER reject_migration_event", database: fixture.database)
        let retry = try await reopened.queueTintaMigration(backup: fixture.backup, admission: fixture.admission,
            previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
        let mutations = try await reopened.tintaMigrationMutations(retry)
        XCTAssertEqual(mutations, fixture.incoming)
        let pending = try await reopened.pendingTintaMigration(reader: retry.admission.reader, generation: retry.admission.merge.generation)
        XCTAssertEqual(pending, retry)
    }
    func testQueueAndRunnerRejectChangedBindingsOverlappingJobsAndStaleTransitions() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let admission = fixture.admission
        var badHash = admission.backupManifest; badHash[0] ^= 1
        let corrupt = try TintaMigrationAdmission(merge: admission.merge, course: admission.course, resource: admission.resource,
            backupTransaction: admission.backupTransaction, reader: admission.reader, backupManifest: badHash)
        do {
            _ = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: corrupt,
                previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
            XCTFail("Changed reviewed manifest accepted")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let job = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: admission,
            previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
        do {
            _ = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: admission,
                previous: [], incoming: fixture.incoming.reversed(), inventory: fixture.inventory, vault: fixture.vault)
            XCTFail("Frozen delivery order changed")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        let merge = try JournalMergeDeclaration(generation: admission.merge.generation, transaction: Data(repeating: 99, count: 16),
            owner: admission.merge.owner, previous: admission.merge.previous, merged: admission.merge.merged)
        let competing = try TintaMigrationAdmission(merge: merge, course: admission.course, resource: admission.resource,
            backupTransaction: admission.backupTransaction, reader: admission.reader, backupManifest: admission.backupManifest)
        do {
            _ = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: competing,
                previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
            XCTFail("Overlapping migration accepted")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        let wire = MigrationWire(admission, incoming: fixture.incoming, fault: "")
        do {
            _ = try await TintaMigrationRunner().run(transaction: job.id, reader: admission.reader,
                generation: admission.merge.generation, owner: Data(repeating: 99, count: 16), transport: wire, library: fixture.library)
            XCTFail("Wrong installation used migration job")
        } catch { XCTAssertEqual(error as? ReaderSessionError, .wrongReader) }
        let untouched = try await fixture.library.tintaMigrationJob(job.id); XCTAssertEqual(untouched, job)
        let trace = await wire.trace(); XCTAssertTrue(trace.isEmpty)
        do {
            _ = try await fixture.library.updateTintaMigration(job, phase: .completed, acknowledgedCount: admission.merge.merged.count)
            XCTFail("Skipped admission/commit phases")
        } catch { XCTAssertEqual(error as? StoreError, .invalidTransition) }
        let admitting = try await fixture.library.updateTintaMigration(job, phase: .admitting, acknowledgedCount: job.acknowledgedCount)
        do {
            _ = try await fixture.library.updateTintaMigration(job, phase: .admitting, acknowledgedCount: job.acknowledgedCount)
            XCTFail("Stale job owner advanced state")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        let unchanged = try await fixture.library.tintaMigrationJob(job.id); XCTAssertEqual(unchanged, admitting)
        _ = try await fixture.library.importReaderJournal(fixture.incoming, reader: admission.reader,
            generation: admission.merge.generation, frontier: admission.merge.merged.frontier, count: admission.merge.merged.count)
        do {
            _ = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: competing,
                previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
            XCTFail("Stale exported baseline accepted for a new job")
        } catch { XCTAssertEqual(error as? HistoryError, .staleFrontier) }
        let retry = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: admission,
            previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
        XCTAssertEqual(retry, admitting)
    }
    func testSchema33UpgradePreservesReviewedBackupAndImportedHistory() async throws {
        let fixture = try await fixture()
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        try sql("DROP TABLE reader_import_filenames; DROP TABLE reader_import_jobs; DROP TABLE journal_merge_events; DROP TABLE journal_merge_jobs; DROP TABLE legacy_preference_imports; DROP TABLE tinta_migration_events; DROP TABLE tinta_migration_jobs; PRAGMA user_version=33", database: fixture.database)
        let reopened = try LibraryStore(url: fixture.database)
        let events = try await reopened.syncEvents(); XCTAssertEqual(events.count, fixture.incoming.count)
        let backups = try await reopened.legacyBackupIDs(reader: fixture.admission.reader,
            generation: fixture.admission.merge.generation, course: fixture.admission.course)
        XCTAssertEqual(backups, [fixture.backup])
        let job = try await reopened.queueTintaMigration(backup: fixture.backup, admission: fixture.admission,
            previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
        XCTAssertEqual(job.phase, .queued)
    }

    func testCanonicalInstallationsAndMigrationJobsCannotOverlapInEitherOrder() async throws {
        for migrationFirst in [false, true] {
            let fixture = try await fixture()
            defer { try? FileManager.default.removeItem(at: fixture.root) }
            let admission = fixture.admission
            let content = try ContentID(admission.resource.map { String(format: "%02x", $0) }.joined())
            let snapshot = try await fixture.library.persistTintaDerivedInstallation(content: content, vault: fixture.vault,
                studyDay: 0, storageGeneration: admission.merge.generation, snapshotIdentity: Data(repeating: 9, count: 16), revision: 1)
            if migrationFirst {
                _ = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: admission,
                    previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
                do {
                    _ = try await fixture.library.enqueueTintaInstallation(manifest: snapshot, inventory: fixture.inventory,
                                                                           owner: admission.merge.owner)
                    XCTFail("Snapshot queued over a migration")
                } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
            } else {
                _ = try await fixture.library.enqueueTintaInstallation(manifest: snapshot, inventory: fixture.inventory,
                                                                       owner: admission.merge.owner)
                do {
                    _ = try await fixture.library.queueTintaMigration(backup: fixture.backup, admission: admission,
                        previous: [], incoming: fixture.incoming, inventory: fixture.inventory, vault: fixture.vault)
                    XCTFail("Migration queued over a snapshot")
                } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
            }
        }
    }

}

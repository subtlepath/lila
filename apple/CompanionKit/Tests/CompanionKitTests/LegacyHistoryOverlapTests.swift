import Foundation
import XCTest
import CSQLite
@testable import CompanionKit

final class LegacyHistoryOverlapTests: XCTestCase, @unchecked Sendable {
    private func sql(_ url: URL, _ statement: String) throws {
        var handle: OpaquePointer?
        guard sqlite3_open(url.path, &handle) == SQLITE_OK else { throw StoreError.invalidValue }
        defer { sqlite3_close(handle) }
        guard sqlite3_exec(handle, statement, nil, nil, nil) == SQLITE_OK else { throw StoreError.invalidValue }
    }
    private func backup(vault: ContentVault, root: URL, reader: UInt8, grades: [UInt8],
                        course: UInt8 = 7, retention: UInt16 = 900) async throws -> ContentID {
        var journal = Data()
        var item = try ScheduledItem(uid: 1)
        for (index, grade) in grades.enumerated() {
            let day = UInt16(10 + index)
            journal.appendLittleEndian(1, count: 4); journal.appendLittleEndian(UInt64(100 + index), count: 4)
            journal.appendLittleEndian(UInt64(day), count: 2); journal.append(contentsOf: [grade, 4])
            item = try item.reviewed(grade: grade, day: day, configuration: SchedulerConfiguration())
        }
        var header = Data("TIS1".utf8)
        header.appendLittleEndian(1, count: 2); header.appendLittleEndian(80, count: 2)
        header.appendLittleEndian(1, count: 4); header.appendLittleEndian(1, count: 4)
        header.appendLittleEndian(UInt64(grades.count), count: 4); header.append(Data(count: 56))
        header.appendLittleEndian(UInt64(legacyCRC32(header)), count: 4)
        var items = Data(count: 1024); items.replaceSubrange(0..<80, with: header); items.append(item.bytes)
        let values: [LegacyBackupRole: Data] = [.reviews: journal, .items: items,
            .profile: legacyProfileFixture(retention: retention), .usage: Data([reader, 42])]
        var sources: [LegacyBackupRole: URL] = [:], files: [LegacyBackupFile] = []
        files.reserveCapacity(values.count)
        for (role, bytes) in values {
            let url = root.appendingPathComponent(UUID().uuidString); try bytes.write(to: url)
            sources[role] = url
            let object = try await vault.importFile(url)
            files.append(LegacyBackupFile(role: role, id: object.id, length: object.length))
        }
        let manifest = try LegacyBackupManifest(reader: Data(repeating: reader, count: 16),
            generation: Data(repeating: 2, count: 16), course: Data(repeating: course, count: 16), files: files)
        return try await vault.preserveLegacyBackup(manifest, sources: sources).id
    }
    func testExactLearnerMatchesPreserveIndependentDiagnosticLogs() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let first = try await backup(vault: vault, root: root, reader: 1, grades: [3, 4])
        let second = try await backup(vault: vault, root: root, reader: 2, grades: [3, 4])
        XCTAssertNotEqual(first, second)
        let forward = try await vault.legacyHistoryOverlap(first: first, second: second)
        XCTAssertEqual(forward?.evidence, .identicalLearnerFiles)
        XCTAssertEqual(forward?.firstReader, Data(repeating: 1, count: 16))
        XCTAssertEqual(forward?.secondReader, Data(repeating: 2, count: 16))
        let reverse = try await vault.legacyHistoryOverlap(first: second, second: first)
        XCTAssertEqual(reverse?.evidence, forward?.evidence)
        let left = try await vault.verifiedLegacyBackup(first), right = try await vault.verifiedLegacyBackup(second)
        let leftUsage = try XCTUnwrap(left.files.first { $0.role == .usage })
        let rightUsage = try XCTUnwrap(right.files.first { $0.role == .usage })
        XCTAssertNotEqual(leftUsage.id, rightUsage.id)
        let leftObject = try await vault.verifiedObject(leftUsage.id), rightObject = try await vault.verifiedObject(rightUsage.id)
        XCTAssertEqual(try Data(contentsOf: leftObject.url), Data([1, 42]))
        XCTAssertEqual(try Data(contentsOf: rightObject.url), Data([2, 42]))
    }
    func testPrefixesRemainEvidenceAndDistinctCoursesDoNotOverlap() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let first = try await backup(vault: vault, root: root, reader: 1, grades: [3, 4, 2])
        let divergent = try await backup(vault: vault, root: root, reader: 2, grades: [3, 1])
        let prefix = try await backup(vault: vault, root: root, reader: 3, grades: [3, 4])
        let independent = try await backup(vault: vault, root: root, reader: 4, grades: [4])
        let otherCourse = try await backup(vault: vault, root: root, reader: 5, grades: [3, 4, 2], course: 8)
        let a = try await vault.legacyHistoryOverlap(first: first, second: divergent)
        let b = try await vault.legacyHistoryOverlap(first: first, second: prefix)
        let c = try await vault.legacyHistoryOverlap(first: first, second: independent)
        let d = try await vault.legacyHistoryOverlap(first: first, second: otherCourse)
        XCTAssertEqual(a?.evidence, .sharedReviewPrefix(records: 1))
        XCTAssertEqual(b?.evidence, .sharedReviewPrefix(records: 2))
        XCTAssertNil(c); XCTAssertNil(d)
        do { _ = try await vault.legacyHistoryOverlap(first: first, second: first); XCTFail("same backup compared") }
        catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        do { _ = try await store.legacyOverlapCandidates(backup: first, vault: vault); XCTFail("unregistered source accepted") }
        catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        for id in [first, divergent, prefix, independent, otherCourse] { try await store.registerLegacyBackup(id, vault: vault) }
        let overlaps = try await store.legacyOverlapCandidates(backup: first, vault: vault)
        XCTAssertEqual(Set(overlaps.map(\.secondBackup)), [divergent, prefix])
        let events = try await store.syncEvents(); XCTAssertTrue(events.isEmpty)
    }
    func testMigrationPreparationRequiresExplicitIndependentDecisionAndChecksBackupSet() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let first = try await backup(vault: vault, root: root, reader: 1, grades: [3])
        let second = try await backup(vault: vault, root: root, reader: 2, grades: [3, 4])
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { workspace.deleteLastPathComponent() }
        let pack = try await vault.importFile(workspace.appendingPathComponent("test/tinta/fixtures/mini.pack"))
        let course = Data(repeating: 7, count: 16), origin = Data(repeating: 3, count: 16)
        let database = root.appendingPathComponent("library.sqlite")
        let guarded = try LibraryStore(url: database, random: { _ in throw CredentialError.invalidRandom })
        for id in [first, second] { try await guarded.registerLegacyBackup(id, vault: vault) }
        do {
            _ = try await guarded.prepareLegacyMigration(backup: second, origin: origin, course: pack.id,
                confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault)
            XCTFail("overlapping history silently migrated")
        } catch let error as LegacyMigrationError {
            guard case let .overlapConfirmationRequired(conflicts) = error else { return XCTFail("wrong error") }
            XCTAssertEqual(conflicts.map(\.secondBackup), [first])
            XCTAssertEqual(conflicts.first?.evidence, .sharedReviewPrefix(records: 1))
        }
        do {
            _ = try await guarded.prepareLegacyMigration(backup: second, origin: origin, course: pack.id,
                confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault,
                confirmedIndependentBackups: [try ContentID(String(repeating: "f", count: 64))])
            XCTFail("unrelated confirmation accepted")
        } catch let error as LegacyMigrationError { XCTAssertEqual(error, .invalidOverlapDecision) }
        let third = try await backup(vault: vault, root: root, reader: 3, grades: [4])
        try await guarded.registerLegacyBackup(third, vault: vault)
        do {
            _ = try await guarded.reserveLegacyMigration(backup: second, origin: origin, resource: pack.id.digest,
                configuration: SchedulerConfiguration(), expectedBackups: [first, second])
            XCTFail("stale preview reserved an epoch")
        } catch let error as LegacyMigrationError { XCTAssertEqual(error, .staleBackupSet) }
        let store = try LibraryStore(url: database, random: { Data(repeating: 7, count: $0) })
        let plan = try await store.prepareLegacyMigration(backup: second, origin: origin, course: pack.id,
            confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault,
            confirmedIndependentBackups: [first])
        let inserted = try await store.installLegacyMigration(plan, backup: second, vault: vault)
        XCTAssertEqual(inserted, 2)
        let restored = try await store.prepareLegacyMigration(backup: second, origin: origin, course: pack.id,
            confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault)
        XCTAssertEqual(restored.mutations, plan.mutations)
        let duplicate = try await store.installLegacyMigration(restored, backup: second, vault: vault)
        XCTAssertEqual(duplicate, 0)
    }
    func testDraftPersistsChoicesWithoutReservingAndRejectsNewBackupCohort() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let first = try await backup(vault: vault, root: root, reader: 1, grades: [3])
        let second = try await backup(vault: vault, root: root, reader: 2, grades: [3, 4])
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { workspace.deleteLastPathComponent() }
        let pack = try await vault.importFile(workspace.appendingPathComponent("test/tinta/fixtures/mini.pack"))
        let database = root.appendingPathComponent("library.sqlite"), course = Data(repeating: 7, count: 16)
        let store = try LibraryStore(url: database, random: { _ in throw CredentialError.invalidRandom })
        for id in [first, second] { try await store.registerLegacyBackup(id, vault: vault) }
        let saved = try await store.saveLegacyMigrationDraft(backup: second, course: pack.id,
            confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault,
            confirmedIndependentBackups: [first])
        XCTAssertTrue(saved)
        let repeated = try await store.saveLegacyMigrationDraft(backup: second, course: pack.id,
            confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault,
            confirmedIndependentBackups: [first])
        XCTAssertFalse(repeated)
        let events = try await store.syncEvents(); XCTAssertTrue(events.isEmpty)
        let reopened = try LibraryStore(url: database, random: { Data(repeating: 7, count: $0) })
        let restoredValue = try await reopened.legacyMigrationDraft(second)
        let restored = try XCTUnwrap(restoredValue)
        XCTAssertEqual(restored.independentBackups, [first]); XCTAssertEqual(restored.course, pack.id)
        let third = try await backup(vault: vault, root: root, reader: 3, grades: [4])
        try await reopened.registerLegacyBackup(third, vault: vault)
        do {
            _ = try await reopened.prepareLegacyMigration(backup: second, origin: Data(repeating: 3, count: 16), vault: vault)
            XCTFail("stale draft prepared")
        } catch let error as LegacyMigrationError { XCTAssertEqual(error, .staleBackupSet) }
        _ = try await reopened.saveLegacyMigrationDraft(backup: second, course: pack.id,
            confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault,
            confirmedIndependentBackups: [first])
        let plan = try await reopened.prepareLegacyMigration(backup: second, origin: Data(repeating: 3, count: 16), vault: vault)
        let inserted = try await reopened.installLegacyMigration(plan, backup: second, vault: vault)
        XCTAssertEqual(inserted, 2)
        let late = try await backup(vault: vault, root: root, reader: 4, grades: [3, 4])
        try await reopened.registerLegacyBackup(late, vault: vault)
        let retry = try await reopened.prepareLegacyMigration(backup: second, origin: Data(repeating: 3, count: 16), vault: vault)
        XCTAssertEqual(retry.mutations, plan.mutations)
        let duplicate = try await reopened.installLegacyMigration(retry, backup: second, vault: vault)
        XCTAssertEqual(duplicate, 0)
        try sql(database, "UPDATE legacy_migration_drafts SET payload=X'00';")
        do { _ = try await reopened.legacyMigrationDraft(second); XCTFail("malformed draft loaded") }
        catch {}
    }
    func testConfirmedSharedHistorySurvivesUpgradeRollbackAndRestartWithoutDuplicateEvents() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let canonical = try await backup(vault: vault, root: root, reader: 1, grades: [3, 4])
        let copied = try await backup(vault: vault, root: root, reader: 2, grades: [3, 4])
        let partial = try await backup(vault: vault, root: root, reader: 3, grades: [3])
        let prepared = try await backup(vault: vault, root: root, reader: 4, grades: [3, 4])
        let database = root.appendingPathComponent("library.sqlite")
        let store = try LibraryStore(url: database, random: { Data(repeating: 7, count: $0) })
        for id in [canonical, copied, partial, prepared] { try await store.registerLegacyBackup(id, vault: vault) }
        do { _ = try await store.confirmSharedLegacyBackup(copied, canonical: canonical, vault: vault); XCTFail("uninstalled root accepted") }
        catch let error as LegacySharedHistoryError { XCTAssertEqual(error, .canonicalNotInstalled) }
        let context = try await store.reserveLegacyMigration(backup: canonical, origin: Data(repeating: 3, count: 16),
            resource: Data(repeating: 5, count: 32), configuration: SchedulerConfiguration())
        let plan = try await vault.legacyMigrationPlan(backup: canonical, context: context)
        _ = try await store.installLegacyMigration(plan, backup: canonical, vault: vault)
        let before = try await store.replayTinta()
        try sql(database, "DROP TABLE journal_merge_events; DROP TABLE journal_merge_jobs; DROP TABLE legacy_preference_imports; DROP TABLE tinta_migration_events; DROP TABLE tinta_migration_jobs; DROP TABLE removal_jobs; DROP TABLE job_font_destinations; DROP TABLE firmware_installations; DROP TABLE course_switch_confirmations; DROP TABLE legacy_backup_jobs; DROP TABLE reader_journal_baselines; DROP TABLE firmware_assets; DROP TABLE tinta_installation_queue; DROP TABLE tinta_artifacts; DROP TABLE job_transfer_declarations; DROP TABLE cloud_visibility_receipts; DROP TABLE library_visibility_events; DROP TABLE cloud_course_receipts; DROP TABLE cloud_course_bindings; DROP TABLE cloud_content_receipts; DROP TABLE cloud_journal_receipts; DROP TABLE legacy_migration_drafts; DROP TABLE legacy_shared_backups; PRAGMA user_version=13;")
        let upgraded = try LibraryStore(url: database)
        let upgradedEvents = try await upgraded.syncEvents(); XCTAssertEqual(upgradedEvents.count, plan.mutations.count)
        try sql(database, "CREATE TRIGGER reject_shared BEFORE INSERT ON legacy_shared_backups BEGIN SELECT RAISE(ABORT,'injected'); END;")
        do { _ = try await upgraded.confirmSharedLegacyBackup(copied, canonical: canonical, vault: vault); XCTFail("failed receipt committed") }
        catch let error as StoreError { guard case .database = error else { return XCTFail("wrong error") } }
        let absent = try await upgraded.sharedLegacyCanonical(copied); XCTAssertNil(absent)
        try sql(database, "DROP TRIGGER reject_shared;")
        let confirmed = try await upgraded.confirmSharedLegacyBackup(copied, canonical: canonical, vault: vault)
        XCTAssertTrue(confirmed)
        let reopened = try LibraryStore(url: database, random: { Data(repeating: 6, count: $0) })
        let restored = try await reopened.sharedLegacyCanonical(copied); XCTAssertEqual(restored, canonical)
        let repeated = try await reopened.confirmSharedLegacyBackup(copied, canonical: canonical, vault: vault)
        XCTAssertFalse(repeated)
        let events = try await reopened.syncEvents(); XCTAssertEqual(events.count, plan.mutations.count)
        let after = try await reopened.replayTinta(); XCTAssertEqual(after.items, before.items)
        XCTAssertEqual(after.studyTotals, before.studyTotals)
        do {
            _ = try await reopened.reserveLegacyMigration(backup: copied, origin: Data(repeating: 9, count: 16),
                resource: context.resource, configuration: context.configuration)
            XCTFail("shared backup reserved duplicate events")
        } catch let error as LegacySharedHistoryError { XCTAssertEqual(error, .alreadyShared(canonical)) }
        do { _ = try await reopened.installLegacyMigration(plan, backup: copied, vault: vault); XCTFail("shared backup installed duplicate events") }
        catch let error as LegacySharedHistoryError { XCTAssertEqual(error, .alreadyShared(canonical)) }
        do { _ = try await reopened.confirmSharedLegacyBackup(partial, canonical: canonical, vault: vault); XCTFail("prefix silently deduplicated") }
        catch let error as LegacySharedHistoryError { XCTAssertEqual(error, .notIdentical) }
        _ = try await reopened.reserveLegacyMigration(backup: prepared, origin: Data(repeating: 8, count: 16),
            resource: context.resource, configuration: context.configuration)
        do { _ = try await reopened.confirmSharedLegacyBackup(prepared, canonical: canonical, vault: vault); XCTFail("prepared backup deduplicated") }
        catch let error as LegacySharedHistoryError { XCTAssertEqual(error, .alreadyPrepared) }
        do { _ = try await reopened.confirmSharedLegacyBackup(copied, canonical: prepared, vault: vault); XCTFail("receipt retargeted") }
        catch let error as LegacySharedHistoryError { XCTAssertEqual(error, .alreadyShared(canonical)) }
        let identity = plan.mutations[0].event.identity.storageKey.map { String(format: "%02x", $0) }.joined()
        try sql(database, "DELETE FROM sync_events WHERE identity=X'\(identity)';")
        do { _ = try await reopened.sharedLegacyCanonical(copied); XCTFail("damaged canonical events accepted") }
        catch let error as LegacySharedHistoryError { XCTAssertEqual(error, .invalidReceipt) }
    }
}

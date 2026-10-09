import Foundation
import XCTest
import CSQLite
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

final class CoursePackTests: XCTestCase, @unchecked Sendable {
    func testTintaQueueMigrationPreservesBothPhasesAndRollsBackFailedCopy() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        _ = try LibraryStore(url: url)
        var repository = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        for _ in 0 ..< 4 { repository.deleteLastPathComponent() }
        let payload = try Data(contentsOf: repository.appendingPathComponent("protocol/fixtures/TintaDerivedManifest-v1.fixture"))
        let manifest = try ContentID(SHA256.hash(data: payload).map { String(format: "%02x", $0) }.joined())
        let payloadHex = payload.map { String(format: "%02x", $0) }.joined()
        let owner = Data(repeating: 9, count: 16), generation = Data(repeating: 1, count: 16)
        let transactions = [UUID(), UUID()]
        var connection: OpaquePointer?
        XCTAssertEqual(sqlite3_open(url.path, &connection), SQLITE_OK)
        defer { sqlite3_close(connection) }
        func sql(_ statement: String) throws {
            guard sqlite3_exec(connection, statement, nil, nil, nil) == SQLITE_OK else { throw StoreError.invalidValue }
        }
        func scalar(_ statement: String) throws -> Int32 {
            var query: OpaquePointer?
            guard sqlite3_prepare_v2(connection, statement, -1, &query, nil) == SQLITE_OK else { throw StoreError.invalidValue }
            defer { sqlite3_finalize(query) }
            guard sqlite3_step(query) == SQLITE_ROW else { throw StoreError.invalidValue }
            return sqlite3_column_int(query, 0)
        }
        try sql("""
            DROP TABLE reader_import_filenames; DROP TABLE reader_import_jobs; DROP TABLE journal_merge_events; DROP TABLE journal_merge_jobs; DROP TABLE legacy_preference_imports; DROP TABLE tinta_migration_events; DROP TABLE tinta_migration_jobs; DROP TABLE removal_jobs; DROP TABLE job_font_destinations; DROP TABLE firmware_installations; DROP TABLE course_switch_confirmations; DROP TABLE legacy_backup_jobs;
            DROP TABLE reader_journal_baselines;
            DROP TABLE firmware_assets;
            DROP TABLE tinta_installation_queue;
            CREATE TABLE tinta_installation_queue(transaction_id TEXT PRIMARY KEY NOT NULL CHECK(length(transaction_id)=36),
                manifest TEXT NOT NULL REFERENCES tinta_artifacts(manifest), reader BLOB NOT NULL CHECK(length(reader)=16),
                generation BLOB NOT NULL CHECK(length(generation)=16), owner BLOB NOT NULL CHECK(length(owner)=16),
                phase TEXT NOT NULL DEFAULT 'queued' CHECK(phase IN ('queued','staging')), UNIQUE(reader,generation));
            INSERT INTO tinta_artifacts VALUES('\(manifest.hex)',X'\(payloadHex)');
            PRAGMA user_version=25;
            """)
        for (index, phase) in [TintaInstallationPhase.queued, .staging].enumerated() {
            let reader = String(repeating: index == 0 ? "02" : "03", count: 16)
            try sql("INSERT INTO tinta_installation_queue VALUES('\(transactions[index].uuidString)','\(manifest.hex)',X'\(reader)',X'\(String(repeating: "01", count: 16))',X'\(String(repeating: "09", count: 16))','\(phase.rawValue)');")
        }
        try sql("PRAGMA ignore_check_constraints=ON; UPDATE tinta_installation_queue SET owner=zeroblob(17) WHERE phase='staging'; PRAGMA ignore_check_constraints=OFF;")
        XCTAssertThrowsError(try LibraryStore(url: url))
        XCTAssertEqual(try scalar("PRAGMA user_version"), 25)
        XCTAssertEqual(try scalar("SELECT count(*) FROM tinta_installation_queue"), 2)
        XCTAssertEqual(try scalar("SELECT count(*) FROM sqlite_master WHERE name='tinta_installation_queue_v26'"), 0)
        try sql("UPDATE tinta_installation_queue SET owner=X'\(String(repeating: "09", count: 16))' WHERE phase='staging';")
        let migrated = try LibraryStore(url: url)
        XCTAssertEqual(try scalar("PRAGMA user_version"), 40)
        for (index, phase) in [TintaInstallationPhase.queued, .staging].enumerated() {
            let pending = try await migrated.pendingTintaInstallation(reader: Data(repeating: UInt8(index + 2), count: 16), generation: generation)
            XCTAssertEqual(pending, PendingTintaInstallation(transaction: transactions[index], manifest: manifest,
                reader: Data(repeating: UInt8(index + 2), count: 16), storageGeneration: generation, owner: owner, phase: phase))
        }
        let retained = try await migrated.retainedTintaArtifactObjects()
        XCTAssertTrue(retained.contains(manifest))
    }
    func testLibraryInstallationRequiresAssociationAndBindsJournal() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"), random: { Data(repeating: 9, count: $0) })
        let object = try await vault.importFile(fixture())
        let metadata = try CoursePackInspector.inspect(fixture())
        try await library.putCoursePack(LibraryContent(id: object.id, kind: .course, length: object.length,
            title: "Course", originalFilename: "course.pack", languages: [metadata.locale]), metadata: metadata)
        func build() async throws -> TintaDerivedInstallation {
            try await library.tintaDerivedInstallation(content: object.id, vault: vault, studyDay: 1,
                storageGeneration: Data(repeating: 1, count: 16), snapshotIdentity: Data(repeating: 2, count: 16), revision: 1)
        }
        do { _ = try await build(); XCTFail("Course association required") }
        catch { XCTAssertEqual(error as? StoreError, .missingContent) }
        _ = try await library.associateCourse(object.id, confirmedIdentity: Data(repeating: 7, count: 16))
        let initial = try await build()
        let preference = try await library.appendPreference(origin: Data(repeating: 1, count: 16),
            preference: PreferenceBody(key: .tintaNewPerDay, value: .integer(10)))
        let changed = try await build()
        XCTAssertEqual(initial.files, changed.files)
        XCTAssertNotEqual(initial.frontierHash, changed.frontierHash)
        XCTAssertEqual(changed.frontierHash, try TintaJournalFrontier.digest([preference]))
        do {
            _ = try await library.buildTintaDerivedInstallation(content: object.id) { pack, details, journal in
                _ = try await library.appendPreference(origin: Data(repeating: 1, count: 16),
                    preference: PreferenceBody(key: .tintaNewPerDay, value: .integer(11)))
                return try await vault.tintaDerivedInstallation(content: object.id, course: pack.logicalIdentity,
                    journal: journal, studyDay: 1, storageGeneration: Data(repeating: 1, count: 16),
                    snapshotIdentity: Data(repeating: 2, count: 16), revision: 1,
                    expectedLength: pack.length, expectedDetails: details)
            }
            XCTFail("History changed during build must be rejected")
        } catch { XCTAssertEqual(error as? HistoryError, .staleFrontier) }
        let refreshed = try await build()
        XCTAssertNotEqual(refreshed.frontierHash, changed.frontierHash)
        let artifactID = try await library.persistTintaDerivedInstallation(content: object.id, vault: vault,
            studyDay: 1, storageGeneration: Data(repeating: 1, count: 16),
            snapshotIdentity: Data(repeating: 2, count: 16), revision: 1)
        let restored = try await vault.restoreTintaDerivedInstallation(manifest: artifactID, maximumBytes: 4096)
        XCTAssertEqual(restored, refreshed)
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let references = try await reopened.retainedTintaArtifactObjects()
        XCTAssertTrue(references.contains(artifactID))
        XCTAssertTrue(references.contains(object.id))
        let receipt = try TintaDerivedReceipt(decoding: restored.manifest)
        for file in receipt.files {
            XCTAssertTrue(references.contains(try ContentID(file.hash.map { String(format: "%02x", $0) }.joined())))
        }
        let resumed = try await reopened.restoreRetainedTintaDerivedInstallation(manifest: artifactID, vault: vault,
            storageGeneration: Data(repeating: 1, count: 16), maximumBytes: 4096)
        XCTAssertEqual(resumed, restored)
        do {
            _ = try await reopened.restoreRetainedTintaDerivedInstallation(manifest: artifactID, vault: vault,
                storageGeneration: Data(repeating: 8, count: 16), maximumBytes: 4096)
            XCTFail("Wrong storage generation must be rejected")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let owner = Data(repeating: 5, count: 16)
        let packManifest = try await reopened.courseManifest(object.id)
        let inventory = try ReaderInventory(reader: Data(repeating: 4, count: 16), generation: Data(repeating: 1, count: 16),
            contents: [packManifest], complete: true)
        let incomplete = try ReaderInventory(reader: inventory.reader, generation: inventory.generation,
            contents: inventory.contents, complete: false)
        let missingPack = try ReaderInventory(reader: inventory.reader, generation: inventory.generation,
            contents: [], complete: true)
        let wrongCard = try ReaderInventory(reader: inventory.reader, generation: Data(repeating: 8, count: 16),
            contents: inventory.contents, complete: true)
        let otherPack = try ContentManifest(content: ContentID(String(repeating: "a", count: 64)), kind: .course,
            length: 100, formatVersion: 1, logicalIdentity: Data(repeating: 8, count: 16))
        let ambiguous = try ReaderInventory(reader: inventory.reader, generation: inventory.generation,
            contents: [packManifest, otherPack], complete: true)
        for invalid in [incomplete, missingPack, wrongCard, ambiguous] {
            do {
                _ = try await reopened.enqueueTintaInstallation(manifest: artifactID, inventory: invalid, owner: owner)
                XCTFail("Invalid inventory must not enqueue")
            } catch { }
            let absent = try await reopened.pendingTintaInstallation(reader: invalid.reader, generation: invalid.generation)
            XCTAssertNil(absent)
        }
        for invalidOwner in [Data(), Data(count: 16), Data(count: 17)] {
            do {
                _ = try await reopened.enqueueTintaInstallation(manifest: artifactID, inventory: inventory, owner: invalidOwner)
                XCTFail("Invalid owner must not enqueue")
            } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        }
        let queued = try await reopened.enqueueTintaInstallation(manifest: artifactID, inventory: inventory, owner: owner)
        let anotherReader = try ReaderInventory(reader: Data(repeating: 9, count: 16), generation: inventory.generation,
            contents: inventory.contents, complete: true)
        do {
            _ = try await library.enqueueTintaInstallation(manifest: artifactID, inventory: anotherReader,
                owner: owner, transaction: queued.transaction)
            XCTFail("Transaction cannot be rebound to another reader")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        let anotherPending = try await library.pendingTintaInstallation(reader: anotherReader.reader, generation: anotherReader.generation)
        XCTAssertNil(anotherPending)
        let repeatedQueue = try await library.enqueueTintaInstallation(manifest: artifactID, inventory: inventory, owner: owner)
        XCTAssertEqual(repeatedQueue, queued)
        let pending = try await library.pendingTintaInstallation(reader: inventory.reader, generation: inventory.generation)
        XCTAssertEqual(pending, queued)
        do { _ = try await reopened.releaseTintaArtifact(artifactID); XCTFail("Queued artifact must remain retained") }
        catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        do {
            _ = try await library.enqueueTintaInstallation(manifest: artifactID, inventory: inventory, owner: Data(repeating: 6, count: 16))
            XCTFail("Foreign queue owner must be rejected")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        try await library.cancelQueuedTintaInstallation(queued.transaction, owner: Data(repeating: 6, count: 16))
        let stillPending = try await library.pendingTintaInstallation(reader: inventory.reader, generation: inventory.generation)
        XCTAssertEqual(stillPending, queued)
        try await library.cancelQueuedTintaInstallation(queued.transaction, owner: owner)
        _ = try await library.appendPreference(origin: Data(repeating: 1, count: 16),
            preference: PreferenceBody(key: .tintaNewPerDay, value: .integer(12)))
        do {
            _ = try await reopened.restoreRetainedTintaDerivedInstallation(manifest: artifactID, vault: vault,
                storageGeneration: Data(repeating: 1, count: 16), maximumBytes: 4096)
            XCTFail("Changed journal must reject retained snapshot")
        } catch { XCTAssertEqual(error as? HistoryError, .staleFrontier) }
        let currentID = try await library.persistTintaDerivedInstallation(content: object.id, vault: vault,
            studyDay: 1, storageGeneration: inventory.generation, snapshotIdentity: Data(repeating: 3, count: 16), revision: 2)
        let next = try await library.enqueueTintaInstallation(manifest: currentID, inventory: inventory, owner: owner)
        let claimed = try await library.claimTintaInstallation(next, inventory: inventory)
        XCTAssertEqual(claimed.phase, .staging)
        let claimedAgain = try await reopened.claimTintaInstallation(next, inventory: inventory)
        XCTAssertEqual(claimedAgain, claimed)
        try await reopened.cancelQueuedTintaInstallation(claimed.transaction, owner: owner)
        let durable = try await reopened.pendingTintaInstallation(reader: inventory.reader, generation: inventory.generation)
        XCTAssertEqual(durable, claimed)
        do { _ = try await reopened.releaseTintaArtifact(currentID); XCTFail("Staging artifact must remain retained") }
        catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        let committedInstallation = try await reopened.restoreRetainedTintaDerivedInstallation(manifest: currentID,
            vault: vault, storageGeneration: inventory.generation, maximumBytes: 4096)
        let committing = try await library.prepareTintaInstallationCommit(claimed, inventory: inventory)
        XCTAssertEqual(committing.phase, .committing)
        do {
            _ = try await reopened.claimTintaInstallation(claimed, inventory: inventory)
            XCTFail("Committing request cannot return to staging")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        _ = try await library.appendPreference(origin: Data(repeating: 1, count: 16),
            preference: PreferenceBody(key: .tintaNewPerDay, value: .integer(13)))
        let recoveredCommit = try await reopened.prepareTintaInstallationCommit(claimed, inventory: inventory)
        XCTAssertEqual(recoveredCommit, committing)
        let recoveredInstallation = try await reopened.restoreCommittingTintaInstallation(committing, vault: vault, maximumBytes: 4096)
        XCTAssertEqual(recoveredInstallation, committedInstallation)
        do {
            _ = try await reopened.restoreCommittingTintaInstallation(claimed, vault: vault, maximumBytes: 4096)
            XCTFail("Staging request cannot use commit recovery")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        do {
            _ = try await reopened.restoreRetainedTintaDerivedInstallation(manifest: currentID, vault: vault,
                storageGeneration: inventory.generation, maximumBytes: 4096)
            XCTFail("A new installation must still validate the current frontier")
        } catch { XCTAssertEqual(error as? HistoryError, .staleFrontier) }

        for peer in [Data(repeating: 6, count: 16), Data()] {
            do {
                _ = try await reopened.confirmTintaInstallation(committing, committedManifest: committedInstallation.manifest,
                    reader: peer, generation: inventory.generation, owner: owner)
                XCTFail("Foreign reader must not clear staging request")
            } catch { XCTAssertEqual(error as? ContentReconciliationError, .wrongReader) }
        }
        do {
            _ = try await reopened.confirmTintaInstallation(committing, committedManifest: restored.manifest,
                reader: inventory.reader, generation: inventory.generation, owner: owner)
            XCTFail("A different installation receipt must not clear staging request")
        } catch { XCTAssertEqual(error as? VaultError, .integrity) }
        let stillStaging = try await library.pendingTintaInstallation(reader: inventory.reader, generation: inventory.generation)
        XCTAssertEqual(stillStaging, committing)
        let confirmed = try await reopened.confirmTintaInstallation(committing, committedManifest: committedInstallation.manifest,
            reader: inventory.reader, generation: inventory.generation, owner: owner)
        XCTAssertTrue(confirmed)
        let confirmedAgain = try await library.confirmTintaInstallation(committing, committedManifest: committedInstallation.manifest,
            reader: inventory.reader, generation: inventory.generation, owner: owner)
        XCTAssertFalse(confirmedAgain)
        let finished = try await library.pendingTintaInstallation(reader: inventory.reader, generation: inventory.generation)
        XCTAssertNil(finished)
        do {
            _ = try await reopened.restoreCommittingTintaInstallation(committing, vault: vault, maximumBytes: 4096)
            XCTFail("Completed request cannot restore through pending commit recovery")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }

        let released = try await reopened.releaseTintaArtifact(artifactID)
        XCTAssertTrue(released)
        let releasedAgain = try await reopened.releaseTintaArtifact(artifactID)
        XCTAssertFalse(releasedAgain)
        let remaining = try await reopened.retainedTintaArtifactObjects()
        XCTAssertFalse(remaining.contains(artifactID))
        XCTAssertTrue(remaining.contains(currentID))
        do {
            _ = try await reopened.restoreRetainedTintaDerivedInstallation(manifest: artifactID, vault: vault,
                storageGeneration: Data(repeating: 1, count: 16), maximumBytes: 4096)
            XCTFail("Released artifact must not be resumed")
        } catch { XCTAssertEqual(error as? StoreError, .missingContent) }

        _ = try await library.deleteLibraryContent(object.id)
        do { _ = try await build(); XCTFail("Deleted pack must be rejected") }
        catch { XCTAssertEqual(error as? StoreError, .missingContent) }
    }
    func testVaultInstallationBindsVerifiedPackAndRejectsCorruption() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root)
        let object = try await vault.importFile(fixture())
        let installation = try await vault.tintaDerivedInstallation(content: object.id,
            course: Data(repeating: 7, count: 16), journal: [], studyDay: 42,
            storageGeneration: Data(repeating: 1, count: 16), snapshotIdentity: Data(repeating: 2, count: 16), revision: 1)
        XCTAssertEqual(installation.manifest.subdata(in: 20 ..< 52), object.id.digest)
        let stored = try await vault.storeTintaDerivedInstallation(installation)
        let reopened = try ContentVault(root: root)
        let restored = try await reopened.restoreTintaDerivedInstallation(manifest: stored.id, maximumBytes: 4096)
        XCTAssertEqual(restored, installation)
        let repeated = try await reopened.storeTintaDerivedInstallation(installation)
        XCTAssertEqual(repeated, stored)
        do {
            _ = try await reopened.restoreTintaDerivedInstallation(manifest: stored.id, maximumBytes: 333)
            XCTFail("Oversized restoration must fail")
        } catch { XCTAssertEqual(error as? ProtocolError, .length) }
        let decoded = try TintaDerivedReceipt(decoding: installation.manifest)
        let itemID = try ContentID(decoded.files[0].hash.map { String(format: "%02x", $0) }.joined())
        let itemObject = try await reopened.verifiedObject(itemID)
        try Data([0]).write(to: itemObject.url)
        do {
            _ = try await reopened.restoreTintaDerivedInstallation(manifest: stored.id, maximumBytes: 4096)
            XCTFail("Corrupt stored file must fail")
        } catch { XCTAssertEqual(error as? VaultError, .integrity) }
        XCTAssertEqual(installation.files.studyDay, 42)
        XCTAssertEqual(installation.frontierHash, try TintaJournalFrontier.digest([]))
        try Data([0]).write(to: object.url)
        do {
            _ = try await vault.tintaDerivedInstallation(content: object.id,
                course: Data(repeating: 7, count: 16), journal: [], studyDay: 42,
                storageGeneration: Data(repeating: 1, count: 16), snapshotIdentity: Data(repeating: 2, count: 16), revision: 1)
            XCTFail("Corrupt pack must not produce an installation")
        } catch { XCTAssertEqual(error as? VaultError, .integrity) }
        let invalid = root.appendingPathComponent("invalid.pack")
        try Data([1, 2, 3]).write(to: invalid)
        let invalidObject = try await vault.importFile(invalid)
        do {
            _ = try await vault.tintaDerivedInstallation(content: invalidObject.id,
                course: Data(repeating: 7, count: 16), journal: [], studyDay: 42,
                storageGeneration: Data(repeating: 1, count: 16), snapshotIdentity: Data(repeating: 2, count: 16), revision: 1)
            XCTFail("Hash-valid invalid pack must not produce an installation")
        } catch { }
    }
    func testHistoricalMembershipDistinguishesRetiredAndUnknownItems() throws {
        let legacy = try CoursePackInspector.inspect(fixture())
        XCTAssertNil(legacy.itemHistoryCount)
        for uid in legacy.itemIdentities { XCTAssertTrue(legacy.containsHistoricalItem(uid)) }
        XCTAssertFalse(legacy.containsHistoricalItem(0))
        XCTAssertFalse(legacy.containsHistoricalItem(UInt32.max))
        let metadata = CoursePackMetadata(major: 1, minor: 0, contentVersion: 1, locale: "es",
            itemIdentities: [1, 3], recognitionItems: [1], storyIdentities: [], lessonIdentities: [],
            lessonCount: 0, legacyStoryIdentities: [], itemHistoryCount: 3)
        XCTAssertTrue(metadata.containsHistoricalItem(2))
        XCTAssertFalse(metadata.itemIdentities.contains(2))
        XCTAssertFalse(metadata.containsHistoricalItem(4))
        XCTAssertFalse(metadata.containsHistoricalItem(0))
        XCTAssertFalse(metadata.containsHistoricalItem(UInt32.max))
    }
    func testTransferAdmissionVerifiesObjectsAndCourseSelection() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let object = try await vault.importFile(fixture())
        let metadata = try CoursePackInspector.inspect(fixture())
        try await library.putCoursePack(LibraryContent(id: object.id, kind: .course, length: object.length,
            title: "Course", originalFilename: "course.pack", languages: [metadata.locale]), metadata: metadata)
        let family = Data(repeating: 7, count: 16)
        _ = try await library.associateCourse(object.id, confirmedIdentity: family)
        let manifest = try await library.courseManifest(object.id)
        func inventory(_ contents: [ContentManifest], complete: Bool = true) throws -> ReaderInventory {
            try ReaderInventory(reader: Data(repeating: 1, count: 16), generation: Data(repeating: 2, count: 16),
                                contents: contents, complete: complete)
        }
        let first = try await library.admitCourseTransfer(object.id, inventory: inventory([]), vault: vault)
        XCTAssertEqual(first, .readerValidationRequired)
        let same = try await library.admitCourseTransfer(object.id, inventory: inventory([manifest]), vault: vault)
        XCTAssertEqual(same, .compatible)
        let unknown = try ContentManifest(content: ContentID(String(repeating: "a", count: 64)), kind: .course,
            length: 100, formatVersion: 1, logicalIdentity: family)
        let absent = try await library.admitCourseTransfer(object.id, inventory: inventory([unknown]), vault: vault)
        XCTAssertEqual(absent, .readerValidationRequired)
        let foreign = try ContentManifest(content: unknown.content, kind: .course, length: 100,
            formatVersion: 1, logicalIdentity: Data(repeating: 8, count: 16))
        do {
            _ = try await library.admitCourseTransfer(object.id, inventory: inventory([foreign]), vault: vault)
            XCTFail("different course needs an explicit switch")
        } catch let error as CourseTransferAdmissionError { XCTAssertEqual(error, .differentCourse) }
        do {
            _ = try await library.admitCourseTransfer(object.id, inventory: inventory([manifest, unknown]), vault: vault)
            XCTFail("ambiguous active courses must not be guessed")
        } catch let error as CourseTransferAdmissionError { XCTAssertEqual(error, .multipleActiveCourses) }
        do {
            _ = try await library.admitCourseTransfer(object.id, inventory: inventory([], complete: false), vault: vault)
            XCTFail("partial inventory cannot authorize replacement")
        } catch let error as ContentReconciliationError { XCTAssertEqual(error, .incompleteInventory) }
        var changed = try Data(contentsOf: fixture())
        changed[8] ^= 1
        let source = root.appendingPathComponent("changed.pack")
        try checksummed(changed).write(to: source)
        let update = try await vault.importFile(source)
        let updateMetadata = try CoursePackInspector.inspect(source)
        try await library.putCoursePack(LibraryContent(id: update.id, kind: .course, length: update.length,
            title: "Update", originalFilename: "changed.pack", languages: [updateMetadata.locale]), metadata: updateMetadata)
        _ = try await library.associateCourse(update.id, confirmedIdentity: family)
        let legacy = try await library.admitCourseTransfer(update.id, inventory: inventory([manifest]), vault: vault)
        XCTAssertEqual(legacy, .readerValidationRequired)
        for (length, format) in [(manifest.length + 1, manifest.formatVersion), (manifest.length, UInt32(2))] {
            let inconsistent = try ContentManifest(content: manifest.content, kind: .course, length: length,
                formatVersion: format, logicalIdentity: family)
            do {
                _ = try await library.admitCourseTransfer(update.id, inventory: inventory([inconsistent]), vault: vault)
                XCTFail("known installed bytes must match reader metadata")
            } catch let error as VaultError { XCTAssertEqual(error, .integrity) }
        }
        try Data([1, 2, 3]).write(to: object.url)
        do {
            _ = try await library.admitCourseTransfer(object.id, inventory: inventory([]), vault: vault)
            XCTFail("corrupt candidate must fail before transfer")
        } catch let error as VaultError { XCTAssertEqual(error, .integrity) }
    }
    private func checksummed(_ input: Data) -> Data {
        var bytes = input
        for offset in 20..<24 { bytes[offset] = 0 }
        var crc: UInt32 = 0xffffffff
        for byte in bytes {
            crc ^= UInt32(byte)
            for _ in 0..<8 { crc = (crc >> 1) ^ (crc & 1 == 0 ? 0 : 0xedb88320) }
        }
        crc = ~crc
        for offset in 0..<4 { bytes[20 + offset] = UInt8(truncatingIfNeeded: crc >> (8 * offset)) }
        return bytes
    }
    private func fixture() -> URL {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { root.deleteLastPathComponent() }
        return root.appendingPathComponent("test/tinta/fixtures/mini.pack")
    }
    func testOptionalIdentityHistoryValidation() async throws {
        let original = try Data(contentsOf: fixture())
        let metadata = try CoursePackInspector.inspect(fixture())
        let maximum = Int(try XCTUnwrap(metadata.itemIdentities.max()))
        func integer(_ at: Int, _ width: Int) -> Int {
            (0..<width).reduce(0) { $0 | Int(original[at + $1]) << ($1 * 8) }
        }
        func put(_ data: inout Data, _ at: Int, _ value: Int, width: Int = 4) {
            for index in 0..<width { data[at + index] = UInt8(truncatingIfNeeded: value >> (index * 8)) }
        }
        let oldDirectory = integer(32, 4), count = integer(36, 2)
        let directory = (original.count + 3) & ~3
        let identity = directory + (count + 1) * 16
        var bytes = original
        bytes.append(Data(repeating: 0, count: identity + maximum * 36 - bytes.count))
        bytes.replaceSubrange(directory..<directory + count * 16,
                              with: original[oldDirectory..<oldDirectory + count * 16])
        let entry = directory + count * 16
        bytes.replaceSubrange(entry..<entry + 4, with: Data("IDEN".utf8))
        put(&bytes, entry + 4, identity); put(&bytes, entry + 8, maximum * 36)
        put(&bytes, entry + 12, maximum)
        put(&bytes, 32, directory); put(&bytes, 36, count + 1, width: 2)
        put(&bytes, 16, bytes.count)
        for index in 0..<maximum {
            put(&bytes, identity + index * 36, index + 1)
            bytes[identity + index * 36 + 4] = 1
        }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        try checksummed(bytes).write(to: url)
        let identified = try CoursePackInspector.inspect(url)
        XCTAssertEqual(identified.itemHistoryCount, UInt32(maximum))
        XCTAssertEqual(identified.itemIdentities, metadata.itemIdentities)
        XCTAssertEqual(try CoursePackDetails(identified), try CoursePackDetails(metadata))
        XCTAssertTrue(identified.containsHistoricalItem(UInt32(maximum)))
        XCTAssertFalse(identified.containsHistoricalItem(UInt32(maximum) + 1))
        XCTAssertEqual(try CoursePackInspector.compareItemHistory(current: url, candidate: url), .compatible)
        XCTAssertEqual(try CoursePackInspector.compareItemHistory(current: fixture(), candidate: url), .identityBaseline)
        let candidate = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: candidate) }
        var changedPayload = bytes
        let name = try XCTUnwrap(changedPayload.range(of: Data("Luis".utf8)))
        changedPayload[name.lowerBound] = 82
        try checksummed(changedPayload).write(to: candidate)
        XCTAssertEqual(try CoursePackInspector.compareItemHistory(current: fixture(), candidate: candidate), .missingHistory)
        var changed = bytes; changed[identity + 4] = 2
        try checksummed(changed).write(to: candidate)
        XCTAssertEqual(try CoursePackInspector.compareItemHistory(current: url, candidate: candidate), .reassignedIdentity)
        var otherLanguage = bytes
        otherLanguage.replaceSubrange(24..<32, with: Array("fr".utf8) + Array(repeating: 0, count: 6))
        try checksummed(otherLanguage).write(to: candidate)
        XCTAssertEqual(try CoursePackInspector.compareItemHistory(current: url, candidate: candidate), .differentLanguage)
        var localeCase = bytes
        for index in 24..<32 where (97...122).contains(localeCase[index]) { localeCase[index] -= 32 }
        try checksummed(localeCase).write(to: candidate)
        XCTAssertEqual(try CoursePackInspector.compareItemHistory(current: url, candidate: candidate), .compatible)
        var extended = bytes
        extended.appendLittleEndian(UInt64(maximum + 1), count: 4)
        extended.append(Data(repeating: 3, count: 32))
        put(&extended, entry + 8, (maximum + 1) * 36); put(&extended, entry + 12, maximum + 1)
        put(&extended, 16, extended.count)
        try checksummed(extended).write(to: candidate)
        XCTAssertEqual(try CoursePackInspector.compareItemHistory(current: url, candidate: candidate), .compatible)
        XCTAssertEqual(try CoursePackInspector.compareItemHistory(current: candidate, candidate: url), .removedHistory)
        let retired = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: retired) }
        extended[extended.count - 1] = 4
        try checksummed(extended).write(to: retired)
        XCTAssertEqual(try CoursePackInspector.compareItemHistory(current: candidate, candidate: retired), .reassignedIdentity)
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let family = Data(repeating: 7, count: 16)
        func register(_ source: URL) async throws -> ContentManifest {
            let object = try await vault.importFile(source)
            let metadata = try CoursePackInspector.inspect(source)
            try await library.putCoursePack(LibraryContent(id: object.id, kind: .course, length: object.length,
                title: "Course", originalFilename: "course.pack", languages: [metadata.locale]), metadata: metadata)
            _ = try await library.associateCourse(object.id, confirmedIdentity: family)
            return try await library.courseManifest(object.id)
        }
        func inventory(_ manifest: ContentManifest) throws -> ReaderInventory {
            try ReaderInventory(reader: Data(repeating: 1, count: 16), generation: Data(repeating: 2, count: 16),
                                contents: [manifest], complete: true)
        }
        let legacyManifest = try await register(fixture())
        let originalManifest = try await register(url)
        let extendedManifest = try await register(candidate)
        let retiredManifest = try await register(retired)
        let baseline = try await library.admitCourseTransfer(originalManifest.content,
            inventory: inventory(legacyManifest), vault: vault)
        XCTAssertEqual(baseline, .identityBaseline)
        let addition = try await library.admitCourseTransfer(extendedManifest.content,
            inventory: inventory(originalManifest), vault: vault)
        XCTAssertEqual(addition, .compatible)
        // A queue retaining its pre-update inventory would admit this now-incompatible downgrade.
        let staleAdmission = try await library.admitCourseTransfer(originalManifest.content,
            inventory: inventory(originalManifest), vault: vault)
        XCTAssertEqual(staleAdmission, .compatible)
        for (next, expected) in [(originalManifest, CoursePackInspector.ItemHistoryCompatibility.removedHistory),
                                 (retiredManifest, .reassignedIdentity)] {
            do {
                _ = try await library.admitCourseTransfer(next.content, inventory: inventory(extendedManifest), vault: vault)
                XCTFail("incompatible history must fail preflight")
            } catch let error as CourseTransferAdmissionError { XCTAssertEqual(error, .incompatibleHistory(expected)) }
        }
        var zeroUID = bytes; put(&zeroUID, identity, 0)
        var zeroDigest = bytes; zeroDigest[identity + 4] = 0
        var missing = bytes
        put(&missing, entry + 8, (maximum - 1) * 36); put(&missing, entry + 12, maximum - 1)
        var stride = bytes; put(&stride, entry + 8, maximum * 36 - 1)
        var duplicate = bytes
        duplicate.append(bytes[entry..<entry + 16])
        let relocated = duplicate.count
        duplicate.append(bytes[directory..<identity])
        duplicate.append(bytes[entry..<entry + 16])
        put(&duplicate, 32, relocated); put(&duplicate, 36, count + 2, width: 2)
        put(&duplicate, 16, duplicate.count)
        for invalid in [zeroUID, zeroDigest, missing, stride, duplicate] {
            try checksummed(invalid).write(to: url)
            XCTAssertThrowsError(try CoursePackInspector.inspect(url))
        }
    }
    func testLocaleSyntaxAndHeaderPadding() throws {
        for locale in ["es-MX", "fr", "zh-Hant", "es-419", "abcdefgh"] {
            XCTAssertNoThrow(try CoursePackDetails(major: 1, minor: 1, contentVersion: 1, locale: locale))
        }
        for locale in ["", "es--MX", "-es", "es-", "12-US", "es/MX", "es MX", "abcdefghi", "é"] {
            XCTAssertThrowsError(try CoursePackDetails(major: 1, minor: 1, contentVersion: 1, locale: locale))
        }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        for locale in [Array("es MX".utf8), Array("es--MX".utf8), [101, 115, 0, 77, 88], [255], []] {
            var bytes = try Data(contentsOf: fixture())
            bytes.replaceSubrange(24..<32, with: locale + Array(repeating: 0, count: 8 - locale.count))
            try checksummed(bytes).write(to: url)
            XCTAssertThrowsError(try CoursePackInspector.inspect(url))
        }
    }
    func testExistingPortableFixtureAndImportSurviveRestart() async throws {
        let source = fixture()
        let metadata = try CoursePackInspector.inspect(source)
        XCTAssertEqual(metadata.major, 1)
        XCTAssertEqual(metadata.lessonIdentities, [0x00010001, 0x00010002])
        XCTAssertEqual(metadata.lessonCount, 2)
        XCTAssertEqual(metadata.legacyStoryIdentities, [1083085678, 3533433236, 3033622123])
        XCTAssertEqual(metadata.storyIdentities, [3491721030, 3028353045, 1274730023])
        XCTAssertEqual(metadata.stories.map(\.id), metadata.storyIdentities)
        XCTAssertEqual(metadata.stories.map(\.title), ["En la calle", "En la taquería", "Luis en México"])
        XCTAssertEqual(metadata.stories.map(\.kind), [.dialogue, .dialogue, .story])
        XCTAssertEqual(metadata.stories.map(\.unitNumber), [1, 1, 1])
        XCTAssertEqual(metadata.stories.map(\.lessonNumber), [1, 2, 2])
        XCTAssertFalse(metadata.locale.isEmpty)
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let database = root.appendingPathComponent("library.sqlite")
        let importer = ContentImporter(vault: vault, library: try LibraryStore(url: database))
        let result = try await importer.importCoursePack(source)
        XCTAssertEqual(result.metadata, metadata)
        let reopened = try LibraryStore(url: database)
        let content = try await reopened.content(result.content.id)
        XCTAssertEqual(content?.kind, .course)
        XCTAssertEqual(content?.languages, [metadata.locale])
        let details = try await reopened.coursePackDetails(result.content.id)
        XCTAssertEqual(details, try CoursePackDetails(metadata))
        let object = try await vault.verifiedObject(result.content.id)
        XCTAssertEqual(try Data(contentsOf: object.url), try Data(contentsOf: source))
    }
    func testRejectsVersionTruncationDirectoryAndCRC() throws {
        let original = try Data(contentsOf: fixture())
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        var version = original; version[4] = 2
        var directory = original; directory[32] = 1
        var corrupt = original; corrupt[corrupt.count - 2] ^= 1
        for bytes in [version, directory, corrupt, Data(original.prefix(47)), Data(original.dropLast())] {
            try bytes.write(to: url)
            XCTAssertThrowsError(try CoursePackInspector.inspect(url))
        }
    }
    func testCloudAssetChecksHashLengthAndCourseFormatBeforeMetadataCommit() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let sourceVault = try ContentVault(root: root.appendingPathComponent("source"))
        let object = try await sourceVault.importFile(fixture())
        let content = LibraryContent(id: object.id, kind: .course, length: object.length,
                                     title: "Spanish", originalFilename: "Spanish.pack")
        let descriptor = try CloudContentDescriptor(content: content)
        let roundTrip = try JSONDecoder().decode(CloudContentDescriptor.self, from: JSONEncoder().encode(descriptor))
        XCTAssertEqual(roundTrip, descriptor)
        let targetVault = try ContentVault(root: root.appendingPathComponent("target"))
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let importer = ContentImporter(vault: targetVault, library: library)
        let wrongLength = try CloudContentDescriptor(content: LibraryContent(id: object.id, kind: .course,
            length: object.length + 1, title: "Spanish", originalFilename: "Spanish.pack"))
        do { _ = try await importer.importCloudAsset(object.url, descriptor: wrongLength); XCTFail() }
        catch { XCTAssertEqual(error as? VaultError, .integrity) }
        let before = try await library.libraryContentIDs(); XCTAssertTrue(before.isEmpty)
        do { _ = try await targetVault.verifiedObject(object.id); XCTFail("Mismatched asset published") } catch {}
        let wrongID = try CloudContentDescriptor(content: LibraryContent(id: try ContentID(String(repeating: "0", count: 64)),
            kind: .course, length: object.length, title: "Spanish", originalFilename: "Spanish.pack"))
        do { _ = try await importer.importCloudAsset(object.url, descriptor: wrongID); XCTFail() }
        catch { XCTAssertEqual(error as? VaultError, .integrity) }
        let imported = try await importer.importCloudAsset(object.url, descriptor: descriptor)
        XCTAssertEqual(imported.id, content.id); XCTAssertEqual(imported.originalFilename, "Spanish.pack")
        XCTAssertEqual(imported.languages, [try CoursePackInspector.inspect(fixture()).locale])
        let repeated = try await importer.importCloudAsset(object.url, descriptor: descriptor)
        XCTAssertEqual(repeated, imported)
        var corruptBytes = try Data(contentsOf: fixture()); corruptBytes[20] ^= 1
        let corruptSource = root.appendingPathComponent("corrupt.pack")
        try corruptBytes.write(to: corruptSource)
        let corruptObject = try await sourceVault.importFile(corruptSource)
        let corruptDescriptor = try CloudContentDescriptor(content: LibraryContent(id: corruptObject.id, kind: .course,
            length: corruptObject.length, title: "Corrupt", originalFilename: "corrupt.pack"))
        do { _ = try await importer.importCloudAsset(corruptObject.url, descriptor: corruptDescriptor); XCTFail("Invalid course imported") }
        catch {}
        let afterCorruption = try await library.libraryContentIDs(); XCTAssertEqual(afterCorruption, [content.id])
        do { _ = try await targetVault.verifiedObject(corruptObject.id); XCTFail("Invalid course published") } catch {}
        for filename in ["../Spanish.pack", "folder\\Spanish.pack", "Spanish.epub"] {
            XCTAssertThrowsError(try CloudContentDescriptor(content: LibraryContent(id: object.id, kind: .course,
                length: object.length, title: "Spanish", originalFilename: filename)))
        }
    }
    func testStoryFixtureAndAmbiguousLegacyMarks() throws {
        let metadata = try CoursePackInspector.inspect(fixture())
        var root = fixture()
        for _ in 0..<4 { root.deleteLastPathComponent() }
        let fixtureJSON = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf:
            root.appendingPathComponent("protocol/fixtures/CourseStoryIdentity.json"))) as? [String: Any])
        let stories = try XCTUnwrap(fixtureJSON["stories"] as? [[String: Any]])
        XCTAssertEqual(metadata.storyIdentities, try stories.map { try XCTUnwrap($0["identity"] as? NSNumber).uint32Value })
        func marks(_ key: UInt32) throws -> LegacyMarkLog {
            var record = Data(); record.appendLittleEndian(UInt64(key), count: 4)
            record.append(contentsOf: [1, 0])
            record.appendLittleEndian(UInt64(legacyCRC32(record) & 0xffff), count: 2)
            return try LegacyMarkLog(bytes: Data("TMK1".utf8) + record)
        }
        let key = metadata.legacyStoryIdentities[0]
        XCTAssertEqual(try marks(key).completedStoryIdentities(course: metadata), [metadata.storyIdentities[0]])
        var bytes = try Data(contentsOf: fixture())
        func integer(_ at: Int, _ width: Int) -> Int {
            (0..<width).reduce(0) { $0 | (Int(bytes[at + $1]) << (8 * $1)) }
        }
        let directory = integer(32, 4)
        let entry = try XCTUnwrap((0..<integer(36, 2)).map { directory + $0 * 16 }.first {
            bytes[$0..<$0 + 4] == Data("STOR".utf8)
        })
        let offset = integer(entry + 4, 4), stride = integer(entry + 8, 4) / integer(entry + 12, 4)
        var reordered = bytes
        let firstStory = Data(bytes[offset..<offset + stride])
        reordered.replaceSubrange(offset..<offset + stride, with: Data(bytes[offset + stride..<offset + 2 * stride]))
        reordered.replaceSubrange(offset + stride..<offset + 2 * stride, with: firstStory)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        try checksummed(reordered).write(to: url)
        XCTAssertEqual(try CoursePackInspector.inspect(url).storyIdentities,
                       [metadata.storyIdentities[1], metadata.storyIdentities[0], metadata.storyIdentities[2]])
        bytes.replaceSubrange(offset + stride..<offset + stride + 4, with: Data(bytes[offset..<offset + 4]))
        try checksummed(bytes).write(to: url)
        let duplicateTitle = try CoursePackInspector.inspect(url)
        XCTAssertNotEqual(duplicateTitle.storyIdentities[0], duplicateTitle.storyIdentities[1])
        XCTAssertEqual(duplicateTitle.legacyStoryIdentities[0], duplicateTitle.legacyStoryIdentities[1])
        XCTAssertThrowsError(try marks(key).completedStoryIdentities(course: duplicateTitle)) { error in
            XCTAssertEqual(error as? LegacyMarkLogError, .ambiguousKey(key))
        }
        let candidates = Array(duplicateTitle.storyIdentities.prefix(2))
        XCTAssertEqual(try marks(key).readingConflicts(course: duplicateTitle),
                       [LegacyReadingConflict(legacyKey: key, candidates: candidates)])
        XCTAssertEqual(try marks(key).completedStoryIdentities(course: duplicateTitle,
            confirmedResolutions: [key: [candidates[1]]]), [candidates[1]])
        XCTAssertEqual(try marks(key).completedStoryIdentities(course: duplicateTitle,
            confirmedResolutions: [key: Set(candidates)]), candidates.sorted())
        XCTAssertTrue(try marks(key).completedStoryIdentities(course: duplicateTitle,
            confirmedResolutions: [key: []]).isEmpty)
        for choices: [UInt32: Set<UInt32>] in [[key: [UInt32.max]], [UInt32.max: []]] {
            XCTAssertThrowsError(try marks(key).completedStoryIdentities(course: duplicateTitle,
                confirmedResolutions: choices)) { error in
                    guard case .invalidResolution = error as? LegacyMarkLogError else { return XCTFail("wrong resolution error") }
                }
        }
        XCTAssertThrowsError(try marks(key).completedStoryIdentities(course: metadata,
            confirmedResolutions: [key: []])) { error in
                XCTAssertEqual(error as? LegacyMarkLogError, .invalidResolution(key))
            }
    }
    func testCourseDetailsFailureRollsBackAndSameHashDetailsAreImmutable() async throws {
        let metadata = try CoursePackInspector.inspect(fixture())
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: url)
        let id = try ContentID(String(repeating: "a", count: 64))
        let content = LibraryContent(id: id, kind: .course, length: 1, title: "Course",
                                     originalFilename: "course.pack", languages: [metadata.locale])
        func sql(_ statement: String) throws {
            var connection: OpaquePointer?
            guard sqlite3_open(url.path, &connection) == SQLITE_OK else { throw StoreError.invalidValue }
            defer { sqlite3_close(connection) }
            guard sqlite3_exec(connection, statement, nil, nil, nil) == SQLITE_OK else { throw StoreError.invalidValue }
        }
        try sql("CREATE TRIGGER reject_course BEFORE INSERT ON course_packs BEGIN SELECT RAISE(ABORT,'injected'); END;")
        do {
            try await library.putCoursePack(content, metadata: metadata)
            XCTFail("details failure must reject the whole import")
        } catch { }
        let missing = try await library.content(id)
        XCTAssertNil(missing)
        try sql("DROP TRIGGER reject_course;")
        try await library.putCoursePack(content, metadata: metadata)
        let changed = CoursePackMetadata(major: metadata.major, minor: metadata.minor,
            contentVersion: metadata.contentVersion ^ 1, locale: metadata.locale,
            itemIdentities: metadata.itemIdentities, recognitionItems: metadata.recognitionItems,
            storyIdentities: metadata.storyIdentities, lessonIdentities: metadata.lessonIdentities,
            lessonCount: metadata.lessonCount, legacyStoryIdentities: metadata.legacyStoryIdentities)
        do {
            try await library.putCoursePack(content, metadata: changed)
            XCTFail("same hash must retain its verified edition")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let reopened = try LibraryStore(url: url)
        let details = try await reopened.coursePackDetails(id)
        XCTAssertEqual(details, try CoursePackDetails(metadata))
        let legacyID = try ContentID(String(repeating: "b", count: 64))
        try await reopened.put(LibraryContent(id: legacyID, kind: .course, length: 1,
                                              title: "Older import", originalFilename: "old.pack"))
        let absent = try await reopened.coursePackDetails(legacyID)
        XCTAssertNil(absent)
    }
    func testRejectsChecksummedAmbiguousIdentityTablesAndItemBounds() throws {
        let original = try Data(contentsOf: fixture())
        func integer(_ data: Data, _ at: Int, _ width: Int) -> Int {
            (0 ..< width).reduce(0) { $0 | (Int(data[at + $1]) << (8 * $1)) }
        }
        let directory = integer(original, 32, 4)
        var sections: [String: (Int, Int)] = [:]
        for index in 0 ..< integer(original, 36, 2) {
            let at = directory + index * 16
            let tag = String(decoding: original[at ..< at + 4], as: UTF8.self)
            let size = integer(original, at + 8, 4)
            let count = integer(original, at + 12, 4)
            sections[tag] = (integer(original, at + 4, 4), count == 0 ? 0 : size / count)
        }
        let items = try XCTUnwrap(sections["ITEM"])
        let index = try XCTUnwrap(sections["IUID"])
        var duplicate = original
        duplicate.replaceSubrange(items.0 + items.1 ..< items.0 + items.1 + 4,
                                  with: original[items.0 ..< items.0 + 4])
        var wrongIndex = original; wrongIndex[index.0 + 4] = 0xff; wrongIndex[index.0 + 5] = 0xff
        var prerequisite = original; prerequisite[items.0 + 12] = 0; prerequisite[items.0 + 13] = 0
        var candidateRange = original
        for offset in 16 ..< 20 { candidateRange[items.0 + offset] = 0xff }
        let lessons = try XCTUnwrap(sections["LESS"]), units = try XCTUnwrap(sections["UNIT"])
        var duplicateLesson = original
        duplicateLesson.replaceSubrange(lessons.0 + lessons.1 + 16..<lessons.0 + lessons.1 + 18,
                                        with: original[lessons.0 + 16..<lessons.0 + 18])
        var invalidUnit = original; invalidUnit[lessons.0 + 14] = 0xff; invalidUnit[lessons.0 + 15] = 0xff
        var orphanedLesson = original
        orphanedLesson[units.0 + 16] = 1; orphanedLesson[units.0 + 17] = 0
        var unitRangeOverflow = original
        unitRangeOverflow[units.0 + 14] = 0xff; unitRangeOverflow[units.0 + 15] = 0xff
        var invalidUnitString = original
        for offset in 0..<4 { invalidUnitString[units.0 + offset] = 0xff }
        let stories = try XCTUnwrap(sections["STOR"])
        var duplicateStory = original
        duplicateStory.replaceSubrange(stories.0 + stories.1..<stories.0 + stories.1 + 4,
                                      with: original[stories.0..<stories.0 + 4])
        duplicateStory.replaceSubrange(stories.0 + stories.1 + 14..<stories.0 + stories.1 + 16,
                                      with: original[stories.0 + 14..<stories.0 + 16])
        duplicateStory[stories.0 + stories.1 + 20] = original[stories.0 + 20]
        var invalidTitle = original
        for offset in 0..<4 { invalidTitle[stories.0 + offset] = 0xff }
        var invalidOperands: [Data] = []
        invalidOperands.reserveCapacity(7)
        for kind in UInt8(0)...6 {
            var bytes = original
            bytes[items.0 + 4] = kind
            bytes[items.0 + 8] = 0xff; bytes[items.0 + 9] = 0xff
            invalidOperands.append(bytes)
        }
        var invalidLessonItems = original
        for offset in 8..<12 { invalidLessonItems[lessons.0 + offset] = 0xff }
        var invalidLessonNotes = original
        invalidLessonNotes[lessons.0 + 18] = 0xff; invalidLessonNotes[lessons.0 + 19] = 0xff
        var invalidLessonSentences = original
        invalidLessonSentences[lessons.0 + 24] = 0xff; invalidLessonSentences[lessons.0 + 25] = 0xff
        var invalidDialogue = original
        invalidDialogue[lessons.0 + 22] = 0xfe; invalidDialogue[lessons.0 + 23] = 0xff
        var invalidStoryLines = original
        for offset in 8..<12 { invalidStoryLines[stories.0 + offset] = 0xff }
        var invalidStoryQuestions = original
        invalidStoryQuestions[stories.0 + 16] = 0xff; invalidStoryQuestions[stories.0 + 17] = 0xff
        var invalidStoryLesson = original
        invalidStoryLesson[stories.0 + 14] = 0xfe; invalidStoryLesson[stories.0 + 15] = 0xff
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        for var bytes in [duplicate, wrongIndex, prerequisite, candidateRange, duplicateLesson, invalidUnit,
                          orphanedLesson, unitRangeOverflow, invalidUnitString, duplicateStory, invalidTitle,
                          invalidLessonItems, invalidLessonNotes, invalidLessonSentences, invalidDialogue,
                          invalidStoryLines, invalidStoryQuestions, invalidStoryLesson] + invalidOperands {
            for offset in 20 ..< 24 { bytes[offset] = 0 }
            var crc: UInt32 = 0xffffffff
            for byte in bytes {
                crc ^= UInt32(byte)
                for _ in 0 ..< 8 { crc = (crc >> 1) ^ (crc & 1 == 0 ? 0 : 0xedb88320) }
            }
            crc = ~crc
            for offset in 0 ..< 4 { bytes[20 + offset] = UInt8(truncatingIfNeeded: crc >> (8 * offset)) }
            try bytes.write(to: url)
            XCTAssertThrowsError(try CoursePackInspector.inspect(url)) { error in
                XCTAssertEqual(error as? ImportError, .integrity)
            }
        }
        var intro = original
        intro[units.0 + 12] = 0; intro[units.0 + 13] = 0
        for offset in 20..<24 { intro[offset] = 0 }
        var crc: UInt32 = 0xffffffff
        for byte in intro {
            crc ^= UInt32(byte)
            for _ in 0..<8 { crc = (crc >> 1) ^ (crc & 1 == 0 ? 0 : 0xedb88320) }
        }
        crc = ~crc
        for offset in 0..<4 { intro[20 + offset] = UInt8(truncatingIfNeeded: crc >> (8 * offset)) }
        try intro.write(to: url)
        let introMetadata = try CoursePackInspector.inspect(url)
        XCTAssertEqual(introMetadata.lessonIdentities, [1, 2])
        XCTAssertEqual(introMetadata.stories[0].unitNumber, 0)
    }

    func testDisplayTitleClipsUTF8WithoutChangingFullTitleHash() throws {
        let original = try Data(contentsOf: fixture())
        func integer(_ bytes: Data, _ at: Int, _ width: Int) -> Int {
            (0..<width).reduce(0) { $0 | (Int(bytes[at + $1]) << (8 * $1)) }
        }
        func put(_ bytes: inout Data, _ at: Int, _ value: Int) {
            for index in 0..<4 { bytes[at + index] = UInt8(truncatingIfNeeded: value >> (8 * index)) }
        }
        let directory = integer(original, 32, 4)
        let entries = (0..<integer(original, 36, 2)).map { directory + $0 * 16 }
        let strings = try XCTUnwrap(entries.first { original[$0..<$0 + 4] == Data("STRS".utf8) })
        let stories = try XCTUnwrap(entries.first { original[$0..<$0 + 4] == Data("STOR".utf8) })
        let offset = integer(original, strings + 4, 4), size = integer(original, strings + 8, 4)
        let storyOffset = integer(original, stories + 4, 4)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        var inspected: [CourseStoryContext] = []
        for suffix in ["x", "y"] {
            var payload = Data(original[offset..<offset + size])
            payload.append(Data((String(repeating: "a", count: 255) + "é" + suffix).utf8)); payload.append(0)
            var bytes = original
            while bytes.count % 4 != 0 { bytes.append(0) }
            let newOffset = bytes.count
            bytes.append(payload)
            put(&bytes, strings + 4, newOffset); put(&bytes, strings + 8, payload.count)
            put(&bytes, storyOffset, size); put(&bytes, 16, bytes.count)
            try checksummed(bytes).write(to: url)
            inspected.append(try CoursePackInspector.inspect(url).stories[0])
        }
        XCTAssertEqual(inspected[0].title, String(repeating: "a", count: 255) + "…")
        XCTAssertEqual(inspected[0].title, inspected[1].title)
        XCTAssertTrue(inspected[0].titleIsTruncated)
        XCTAssertNotEqual(inspected[0].id, inspected[1].id)
    }
    func testStringUTF8ValidationCarriesStateAcrossChunks() throws {
        let original = try Data(contentsOf: fixture())
        func integer(_ bytes: Data, _ at: Int, _ width: Int) -> Int {
            (0..<width).reduce(0) { $0 | (Int(bytes[at + $1]) << (8 * $1)) }
        }
        func put(_ bytes: inout Data, _ at: Int, _ value: Int) {
            for index in 0..<4 { bytes[at + index] = UInt8(truncatingIfNeeded: value >> (index * 8)) }
        }
        let directory = integer(original, 32, 4)
        let entry = try XCTUnwrap((0..<integer(original, 36, 2)).map { directory + $0 * 16 }.first {
            original[$0..<$0 + 4] == Data("STRS".utf8)
        })
        let offset = integer(original, entry + 4, 4), size = integer(original, entry + 8, 4)
        XCTAssertLessThan(size, 65535)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        let cases: [(bytes: [UInt8], valid: Bool)] = [
            ([0xe2, 0x82, 0xac], true), ([0xf0, 0x9f, 0x93, 0x96], true),
            ([0xc0, 0x80], false), ([0xed, 0xa0, 0x80], false),
            ([0xf4, 0x90, 0x80, 0x80], false), ([0xe2, 0x82], false),
            ([0x80], false), ([0xf5, 0x80, 0x80, 0x80], false)
        ]
        for test in cases {
            var payload = Data(original[offset..<offset + size])
            payload.append(Data(repeating: 0x61, count: 65535 - payload.count))
            payload.append(contentsOf: test.bytes)
            payload.append(0)
            var bytes = original
            while bytes.count % 4 != 0 { bytes.append(0) }
            let newOffset = bytes.count
            bytes.append(payload)
            put(&bytes, entry + 4, newOffset)
            put(&bytes, entry + 8, payload.count)
            put(&bytes, 16, bytes.count)
            put(&bytes, 20, 0)
            var crc: UInt32 = 0xffffffff
            for byte in bytes {
                crc ^= UInt32(byte)
                for _ in 0..<8 { crc = (crc >> 1) ^ (crc & 1 == 0 ? 0 : 0xedb88320) }
            }
            put(&bytes, 20, Int(~crc))
            try bytes.write(to: url)
            if test.valid {
                XCTAssertEqual(try CoursePackInspector.inspect(url).lessonCount, 2)
            } else {
                XCTAssertThrowsError(try CoursePackInspector.inspect(url)) { error in
                    XCTAssertEqual(error as? ImportError, .integrity)
                }
            }
        }
    }
}

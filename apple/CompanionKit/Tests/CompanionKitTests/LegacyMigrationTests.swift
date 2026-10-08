import Foundation
import XCTest
import CSQLite
@testable import CompanionKit

final class LegacyMigrationTests: XCTestCase, @unchecked Sendable {
    private let course = Data(repeating: 7, count: 16)
    private func record(uid: UInt32 = 1, op: UInt8 = 3, arg: UInt8 = 4) -> Data {
        var bytes = Data(); bytes.reserveCapacity(12)
        bytes.appendLittleEndian(UInt64(uid), count: 4); bytes.appendLittleEndian(100, count: 4)
        bytes.appendLittleEndian(10, count: 2); bytes.append(op); bytes.append(arg)
        return bytes
    }
    private func context(retention: UInt16 = 9000) throws -> LegacyMigrationContext {
        try LegacyMigrationContext(origin: Data(repeating: 1, count: 16), epoch: 99,
                                   generation: Data(repeating: 2, count: 16), resource: Data(repeating: 3, count: 32),
                                   configuration: SchedulerConfiguration(retentionBasisPoints: retention))
    }
    private func itemSnapshot(journalCount: UInt32) throws -> Data {
        var header = Data("TIS1".utf8)
        header.appendLittleEndian(1, count: 2); header.appendLittleEndian(80, count: 2)
        header.appendLittleEndian(1, count: 4); header.appendLittleEndian(1, count: 4)
        header.appendLittleEndian(UInt64(journalCount), count: 4); header.append(Data(count: 56))
        header.appendLittleEndian(UInt64(legacyCRC32(header)), count: 4)
        var bytes = Data(count: 1024); bytes.replaceSubrange(0..<80, with: header)
        bytes.append(try ScheduledItem(uid: 1).bytes)
        return bytes
    }
    func testPendingMigrationEpochCannotBeReservedForAnotherBackup() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        var sources: [LegacyBackupRole: URL] = [:]
        var files: [LegacyBackupFile] = []; files.reserveCapacity(3)
        for role in [LegacyBackupRole.reviews, .items, .profile] {
            let url = root.appendingPathComponent(role.rawValue)
            let bytes = role == .reviews ? record() : role == .items ? try itemSnapshot(journalCount: 1) : legacyProfileFixture()
            try bytes.write(to: url)
            sources[role] = url
            let object = try await vault.importFile(url)
            files.append(LegacyBackupFile(role: role, id: object.id, length: object.length))
        }
        let databaseURL = root.appendingPathComponent("library.sqlite")
        let store = try LibraryStore(url: databaseURL, random: { Data(repeating: 9, count: $0) })
        let firstManifest = try LegacyBackupManifest(reader: Data(repeating: 1, count: 16),
            generation: Data(repeating: 2, count: 16), course: course, files: files)
        let secondManifest = try LegacyBackupManifest(reader: Data(repeating: 8, count: 16),
            generation: firstManifest.generation, course: course, files: files)
        let first = try await store.preserveLegacyBackup(firstManifest, sources: sources, vault: vault)
        let second = try await store.preserveLegacyBackup(secondManifest, sources: sources, vault: vault)
        let origin = Data(repeating: 4, count: 16), resource = Data(repeating: 5, count: 32)
        let configuration = try SchedulerConfiguration()
        let reserved = try await store.reserveLegacyMigration(backup: first, origin: origin,
            resource: resource, configuration: configuration)
        let reopened = try LibraryStore(url: databaseURL, random: { Data(repeating: 9, count: $0) })
        do {
            _ = try await reopened.reserveLegacyMigration(backup: second, origin: origin,
                resource: resource, configuration: configuration)
            XCTFail("Pending migration must own its epoch before any events are installed")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let retry = try await reopened.reserveLegacyMigration(backup: first, origin: origin,
            resource: resource, configuration: configuration)
        XCTAssertEqual(retry.origin, reserved.origin)
        XCTAssertEqual(retry.epoch, reserved.epoch)
        let otherOrigin = Data(repeating: 6, count: 16)
        let independent = try await reopened.reserveLegacyMigration(backup: second, origin: otherOrigin,
            resource: resource, configuration: configuration)
        XCTAssertEqual(independent.epoch, reserved.epoch)
        XCTAssertEqual(independent.origin, otherOrigin)
    }
    func testAtomicInstallationRollbackAndRestartRetry() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let bytes = record() + record(op: 8, arg: 0)
        var sources: [LegacyBackupRole: URL] = [:]
        var files: [LegacyBackupFile] = []; files.reserveCapacity(3)
        for role in [LegacyBackupRole.reviews, .items, .profile] {
            let url = root.appendingPathComponent(role.rawValue)
            try (role == .reviews ? bytes : role == .items ? itemSnapshot(journalCount: 2) : legacyProfileFixture()).write(to: url)
            sources[role] = url
            let object = try await vault.importFile(url)
            files.append(LegacyBackupFile(role: role, id: object.id, length: object.length))
        }
        let manifest = try LegacyBackupManifest(reader: Data(repeating: 1, count: 16),
            generation: Data(repeating: 2, count: 16), course: course, files: files)
        let databaseURL = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: databaseURL, random: { Data(repeating: 9, count: $0) })
        let backup = try await library.preserveLegacyBackup(manifest, sources: sources, vault: vault)
        let listed = try await library.verifiedLegacyBackups(course: course, vault: vault)
        XCTAssertEqual(listed.map(\.id), [backup])
        XCTAssertEqual(listed.first?.manifest, manifest)
        let reopenedListing = try LibraryStore(url: databaseURL)
        let listedAgain = try await reopenedListing.verifiedLegacyBackups(course: course, vault: vault)
        XCTAssertEqual(listedAgain, listed)
        let unrelated = try await library.verifiedLegacyBackups(course: Data(repeating: 99, count: 16), vault: vault)
        XCTAssertTrue(unrelated.isEmpty)
        do {
            _ = try await library.verifiedLegacyBackups(course: Data(), vault: vault)
            XCTFail("Invalid course accepted")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let reserved = try await library.reserveLegacyMigration(backup: backup, origin: Data(repeating: 4, count: 16),
            resource: Data(repeating: 5, count: 32), configuration: SchedulerConfiguration())
        do {
            _ = try await library.appendReadingPosition(origin: reserved.origin, content: reserved.resource,
                anchor: ReadingAnchor(spine: 0, visibleTextOffset: 0))
            XCTFail("Reserved migration epoch reused by local event")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let decodedBackup = try await vault.legacyBackupSnapshot(backup)
        XCTAssertNil(decodedBackup.readings); XCTAssertNil(decodedBackup.starred)
        let starSource = root.appendingPathComponent("starred")
        try Data("TMK1".utf8).write(to: starSource)
        let starObject = try await vault.importFile(starSource)
        var starFiles = files; starFiles.reserveCapacity(4)
        starFiles.append(LegacyBackupFile(role: .starred, id: starObject.id, length: starObject.length))
        let starManifest = try LegacyBackupManifest(reader: manifest.reader, generation: manifest.generation,
            course: manifest.course, files: starFiles)
        var starSources = sources; starSources[.starred] = starSource
        let starBackup = try await vault.preserveLegacyBackup(starManifest, sources: starSources)
        let withStars = try await vault.legacyBackupSnapshot(starBackup.id)
        XCTAssertNotNil(withStars.starred); XCTAssertEqual(withStars.starred?.keys, [])
        try (Data("TMK1".utf8) + Data([1])).write(to: starSource)
        let tornStar = try await vault.importFile(starSource)
        starFiles[starFiles.count - 1] = LegacyBackupFile(role: .starred, id: tornStar.id, length: tornStar.length)
        let tornManifest = try LegacyBackupManifest(reader: manifest.reader, generation: manifest.generation,
            course: manifest.course, files: starFiles)
        let tornBackup = try await vault.preserveLegacyBackup(tornManifest, sources: starSources)
        do { _ = try await vault.legacyMigrationPlan(backup: tornBackup.id, context: reserved); XCTFail("Torn star file ignored") }
        catch { XCTAssertEqual(error as? LegacyMarkLogError, .truncated) }
        let preservedTorn = try await vault.verifiedObject(tornStar.id)
        XCTAssertEqual(try Data(contentsOf: preservedTorn.url), Data("TMK1".utf8) + Data([1]))

        let plan = try await vault.legacyMigrationPlan(backup: backup, context: reserved)
        XCTAssertEqual(plan.mutations.count, 2)
        let inconsistentSnapshot = TintaSnapshot(items: plan.snapshot.items,
            completedLessons: [try TintaSubject(course: course, uid: 42)],
            completedReadings: plan.snapshot.completedReadings, studyTotals: plan.snapshot.studyTotals,
            undoneReviews: plan.snapshot.undoneReviews)
        let inconsistentPlan = LegacyMigrationPlan(originalHash: plan.originalHash, mutations: plan.mutations,
            snapshot: inconsistentSnapshot)
        do {
            _ = try await library.installLegacyMigration(inconsistentPlan, backup: backup, vault: vault)
            XCTFail("Snapshot inconsistent with migration events accepted")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let wrongGeneration = try LegacyMigrationContext(origin: reserved.origin, epoch: reserved.epoch,
            generation: Data(repeating: 8, count: 16), resource: reserved.resource, configuration: reserved.configuration)
        do { _ = try await vault.legacyMigrationPlan(backup: backup, context: wrongGeneration); XCTFail("Changed generation accepted") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let mismatchedSource = root.appendingPathComponent("mismatched-items")
        try itemSnapshot(journalCount: 1).write(to: mismatchedSource)
        let mismatchedObject = try await vault.importFile(mismatchedSource)
        var mismatchedFiles = files.filter { $0.role != .items }
        mismatchedFiles.reserveCapacity(3)
        mismatchedFiles.append(LegacyBackupFile(role: .items, id: mismatchedObject.id, length: mismatchedObject.length))
        let mismatchedManifest = try LegacyBackupManifest(reader: manifest.reader, generation: manifest.generation,
            course: manifest.course, files: mismatchedFiles)
        var mismatchedSources = sources; mismatchedSources[.items] = mismatchedSource
        let mismatchedBackup = try await vault.preserveLegacyBackup(mismatchedManifest, sources: mismatchedSources)
        do { _ = try await vault.legacyMigrationPlan(backup: mismatchedBackup.id, context: reserved); XCTFail("Incomplete snapshot accepted") }
        catch { XCTAssertEqual(error as? LegacyMigrationError, .stateMismatch) }
        var handle: OpaquePointer?
        XCTAssertEqual(sqlite3_open(databaseURL.path, &handle), SQLITE_OK)
        defer { sqlite3_close(handle) }
        XCTAssertEqual(sqlite3_exec(handle, "CREATE TRIGGER reject_install BEFORE INSERT ON legacy_installations BEGIN SELECT RAISE(ABORT,'injected'); END", nil, nil, nil), SQLITE_OK)
        do { _ = try await library.installLegacyMigration(plan, backup: backup, vault: vault); XCTFail("Failed completion committed") }
        catch {}
        let rolledBack = try await library.syncEvents()
        XCTAssertEqual(rolledBack.count, 0)
        XCTAssertEqual(sqlite3_exec(handle, "DROP TRIGGER reject_install", nil, nil, nil), SQLITE_OK)
        let installed = try await library.installLegacyMigration(plan, backup: backup, vault: vault)
        XCTAssertEqual(installed, 2)
        let reopened = try LibraryStore(url: databaseURL)
        let retried = try await reopened.installLegacyMigration(plan, backup: backup, vault: vault)
        XCTAssertEqual(retried, 0)
        let snapshot = try await reopened.replayTinta()
        XCTAssertEqual(snapshot.items[try TintaSubject(course: course, uid: 1)], try ScheduledItem(uid: 1))
        let independent = try LibraryStore(url: databaseURL, random: { Data(repeating: 10, count: $0) })
        let local = try await independent.appendReadingPosition(origin: reserved.origin, content: reserved.resource,
            anchor: ReadingAnchor(spine: 0, visibleTextOffset: 0))
        XCTAssertNotEqual(local.event.identity.epoch, reserved.epoch)
        XCTAssertEqual(local.event.identity.sequence, 1)
        let changed = LegacyMigrationPlan(originalHash: plan.originalHash, mutations: [], snapshot: plan.snapshot)
        do { _ = try await reopened.installLegacyMigration(changed, backup: backup, vault: vault); XCTFail("Changed retry accepted") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
    }
    func testConfirmedAmbiguousReadingChoiceIsReservedAndInstalledExactlyOnce() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { workspace.deleteLastPathComponent() }
        var bytes = try Data(contentsOf: workspace.appendingPathComponent("test/tinta/fixtures/mini.pack"))
        func integer(_ at: Int, _ width: Int) -> Int {
            (0..<width).reduce(0) { $0 | (Int(bytes[at + $1]) << (8 * $1)) }
        }
        let directory = integer(32, 4)
        let entry = try XCTUnwrap((0..<integer(36, 2)).map { directory + $0 * 16 }.first {
            bytes[$0..<$0 + 4] == Data("STOR".utf8)
        })
        let offset = integer(entry + 4, 4), stride = integer(entry + 8, 4) / integer(entry + 12, 4)
        bytes.replaceSubrange(offset + stride..<offset + stride + 4, with: Data(bytes[offset..<offset + 4]))
        bytes.replaceSubrange(20..<24, with: Data(count: 4))
        var checksum = Data(); checksum.appendLittleEndian(UInt64(legacyCRC32(bytes)), count: 4)
        bytes.replaceSubrange(20..<24, with: checksum)
        let candidate = root.appendingPathComponent("course.pack"); try bytes.write(to: candidate)
        let metadata = try CoursePackInspector.inspect(candidate)
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let pack = try await vault.importFile(candidate)
        let key = metadata.legacyStoryIdentities[0]
        var mark = Data(); mark.appendLittleEndian(UInt64(key), count: 4); mark.append(contentsOf: [1, 0])
        mark.appendLittleEndian(UInt64(legacyCRC32(mark) & 0xffff), count: 2)
        let values: [LegacyBackupRole: Data] = [.reviews: record() + record(op: 8, arg: 0),
            .items: try itemSnapshot(journalCount: 2), .profile: legacyProfileFixture(), .readings: Data("TMK1".utf8) + mark]
        var files: [LegacyBackupFile] = []; files.reserveCapacity(values.count)
        var sources: [LegacyBackupRole: URL] = [:]
        for (role, value) in values {
            let url = root.appendingPathComponent(role.rawValue); try value.write(to: url)
            sources[role] = url
            let object = try await vault.importFile(url)
            files.append(LegacyBackupFile(role: role, id: object.id, length: object.length))
        }
        let manifest = try LegacyBackupManifest(reader: Data(repeating: 1, count: 16),
            generation: Data(repeating: 2, count: 16), course: course, files: files)
        let backup = try await vault.preserveLegacyBackup(manifest, sources: sources)
        let conflicts = try await vault.legacyReadingConflicts(backup: backup.id, course: pack.id, confirmedCourseIdentity: course)
        XCTAssertEqual(conflicts, [LegacyReadingConflict(legacyKey: key, candidates: Array(metadata.storyIdentities.prefix(2)))])
        let options = try await vault.legacyReadingOptions(backup: backup.id, course: pack.id, confirmedCourseIdentity: course)
        XCTAssertEqual(options.first?.candidates.map(\.title), ["En la calle", "En la calle"])
        XCTAssertEqual(options.first?.candidates.map(\.lessonNumber), [1, 2])
        let database = root.appendingPathComponent("library.sqlite")
        let unresolved = try LibraryStore(url: database, random: { _ in throw CredentialError.invalidRandom })
        try await unresolved.registerLegacyBackup(backup.id, vault: vault)
        do {
            _ = try await unresolved.prepareLegacyMigration(backup: backup.id, origin: Data(repeating: 3, count: 16),
                course: pack.id, confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault)
            XCTFail("unresolved reading must not reserve an epoch")
        } catch let error as LegacyMarkLogError { XCTAssertEqual(error, .ambiguousKey(key)) }
        let store = try LibraryStore(url: database, random: { Data(repeating: 7, count: $0) })
        let selected = metadata.storyIdentities[1]
        _ = try await store.saveLegacyMigrationDraft(backup: backup.id, course: pack.id,
            confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault,
            confirmedReadingResolutions: [key: [selected]])
        let resumed = try LibraryStore(url: database, random: { Data(repeating: 7, count: $0) })
        let plan = try await resumed.prepareLegacyMigration(backup: backup.id, origin: Data(repeating: 3, count: 16), vault: vault)
        XCTAssertEqual(plan.snapshot.completedReadings, [try TintaSubject(course: course, uid: selected)])
        let loadedReview = try await resumed.legacyMigrationDraft(backup.id)
        let reviewed = try XCTUnwrap(loadedReview)
        _ = try await store.saveLegacyMigrationDraft(backup: backup.id, course: pack.id,
            confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault,
            confirmedReadingResolutions: [key: []])
        do {
            _ = try await resumed.prepareLegacyMigration(backup: backup.id, origin: Data(repeating: 3, count: 16),
                                                         vault: vault, expectedDraft: reviewed)
            XCTFail("Changed review prepared")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        do {
            _ = try await resumed.installLegacyMigration(plan, backup: backup.id, vault: vault, expectedDraft: reviewed)
            XCTFail("Changed review installed")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let unchangedEvents = try await resumed.syncEvents(); XCTAssertTrue(unchangedEvents.isEmpty)
        _ = try await store.saveLegacyMigrationDraft(backup: backup.id, course: pack.id,
            confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault,
            confirmedReadingResolutions: [key: [selected]])
        let inserted = try await store.installLegacyMigration(plan, backup: backup.id, vault: vault)
        XCTAssertEqual(inserted, plan.mutations.count)
        let reopened = try LibraryStore(url: database)
        let repeated = try await reopened.installLegacyMigration(plan, backup: backup.id, vault: vault)
        XCTAssertEqual(repeated, 0)
        let changed = try await reopened.prepareLegacyMigration(backup: backup.id, origin: Data(repeating: 3, count: 16),
            course: pack.id, confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault,
            confirmedReadingResolutions: [key: []])
        do {
            _ = try await reopened.installLegacyMigration(changed, backup: backup.id, vault: vault)
            XCTFail("an installed choice must not be changed by a retry")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let restored = try await reopened.replayTinta(course: course)
        XCTAssertEqual(restored.completedReadings, plan.snapshot.completedReadings)
    }
    func testConfirmedCourseProducesCausalCompletionEvents() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        var fixture = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { fixture.deleteLastPathComponent() }
        let pack = try await vault.importFile(fixture.appendingPathComponent("test/tinta/fixtures/mini.pack"))
        var resource = Data(); resource.reserveCapacity(32)
        for at in stride(from: 0, to: 64, by: 2) {
            let start = pack.id.hex.index(pack.id.hex.startIndex, offsetBy: at)
            resource.append(UInt8(pack.id.hex[start..<pack.id.hex.index(start, offsetBy: 2)], radix: 16)!)
        }
        var profile = legacyProfileFixture(); profile[30] = 1; profile[32] = 1
        var crc = Data(); crc.appendLittleEndian(UInt64(legacyCRC32(Data(profile[0..<39]))), count: 4)
        profile.replaceSubrange(39..<43, with: crc)
        var reading = Data("TMK1".utf8), mark = Data()
        mark.appendLittleEndian(1083085678, count: 4); mark.append(contentsOf: [1, 0])
        mark.appendLittleEndian(UInt64(legacyCRC32(mark) & 0xffff), count: 2); reading.append(mark)
        var star = Data(); star.appendLittleEndian(2, count: 4); star.append(contentsOf: [1, 0])
        star.appendLittleEndian(UInt64(legacyCRC32(star) & 0xffff), count: 2)
        let values: [LegacyBackupRole: Data] = [.reviews: record() + record(op: 8, arg: 0),
            .items: try itemSnapshot(journalCount: 2), .profile: profile, .readings: reading, .starred: Data("TMK1".utf8) + star]
        var sources: [LegacyBackupRole: URL] = [:], files: [LegacyBackupFile] = []
        files.reserveCapacity(values.count)
        for (role, bytes) in values {
            let url = root.appendingPathComponent(role.rawValue); try bytes.write(to: url); sources[role] = url
            let stored = try await vault.importFile(url)
            files.append(LegacyBackupFile(role: role, id: stored.id, length: stored.length))
        }
        let manifest = try LegacyBackupManifest(reader: Data(repeating: 1, count: 16),
            generation: Data(repeating: 2, count: 16), course: course, files: files)
        let backup = try await vault.preserveLegacyBackup(manifest, sources: sources)
        XCTAssertEqual(pack.id.digest, resource)
        let databaseURL = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: databaseURL, random: { Data(repeating: 7, count: $0) })
        try await library.registerLegacyBackup(backup.id, vault: vault)
        let plan = try await library.prepareLegacyMigration(backup: backup.id, origin: Data(repeating: 3, count: 16),
            course: pack.id, confirmedCourseIdentity: course, configuration: SchedulerConfiguration(), vault: vault)
        let context = try await library.reserveLegacyMigration(backup: backup.id, origin: Data(repeating: 3, count: 16),
            resource: resource, configuration: SchedulerConfiguration())
        let imported = try await ContentImporter(vault: vault, library: library).importCoursePack(
            fixture.appendingPathComponent("test/tinta/fixtures/mini.pack"))
        XCTAssertEqual(imported.content.id, pack.id)
        do {
            _ = try await library.associateCourse(pack.id, confirmedIdentity: Data(repeating: 8, count: 16))
            XCTFail("migration reservation must prevent a conflicting course association")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        _ = try await library.associateCourse(pack.id, confirmedIdentity: course)
        let conflictingManifest = try LegacyBackupManifest(reader: manifest.reader, generation: manifest.generation,
            course: Data(repeating: 8, count: 16), files: files)
        let conflictingBackup = try await library.preserveLegacyBackup(conflictingManifest, sources: sources, vault: vault)
        do {
            _ = try await library.reserveLegacyMigration(backup: conflictingBackup, origin: Data(repeating: 4, count: 16),
                resource: resource, configuration: SchedulerConfiguration())
            XCTFail("stored association must prevent a conflicting migration reservation")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        func overwriteAssociation(_ identity: Data) throws {
            var connection: OpaquePointer?
            guard sqlite3_open(databaseURL.path, &connection) == SQLITE_OK else { throw StoreError.invalidValue }
            defer { sqlite3_close(connection) }
            let hex = identity.map { String(format: "%02x", $0) }.joined()
            guard sqlite3_exec(connection, "UPDATE course_associations SET identity=X'\(hex)'", nil, nil, nil) == SQLITE_OK else {
                throw StoreError.invalidValue
            }
        }
        try overwriteAssociation(Data(repeating: 8, count: 16))
        do {
            _ = try await library.installLegacyMigration(plan, backup: backup.id, vault: vault)
            XCTFail("installation must recheck course association before writing events")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let beforeInstallation = try await library.replayTinta()
        XCTAssertTrue(beforeInstallation.items.isEmpty)
        try overwriteAssociation(course)
        let installed = try await library.installLegacyMigration(plan, backup: backup.id, vault: vault)
        XCTAssertEqual(installed, 5)
        let reopened = try LibraryStore(url: databaseURL)
        let restoredPlan = try await reopened.prepareLegacyMigration(backup: backup.id, origin: context.origin,
            course: pack.id, confirmedCourseIdentity: course, configuration: context.configuration, vault: vault)
        XCTAssertEqual(restoredPlan.mutations, plan.mutations)
        let duplicate = try await reopened.installLegacyMigration(restoredPlan, backup: backup.id, vault: vault)
        XCTAssertEqual(duplicate, 0)
        let restored = try await reopened.replayTinta()
        XCTAssertEqual(restored.items, plan.snapshot.items)
        XCTAssertEqual(restored.completedLessons, plan.snapshot.completedLessons)
        XCTAssertEqual(restored.completedReadings, plan.snapshot.completedReadings)
        XCTAssertEqual(plan.mutations.map { $0.event.kind }, [.review, .undoReview, .lessonComplete, .readingComplete, .star])
        XCTAssertEqual(plan.mutations[3].event.ancestors, [plan.mutations[2].event.identity])
        XCTAssertEqual(try TintaBody(mutation: plan.mutations[2]).subject.uid, 0x00010001)
        XCTAssertEqual(plan.snapshot.items[try TintaSubject(course: course, uid: 2)]?.bytes[14], 4)
        do { _ = try await vault.legacyMigrationPlan(backup: backup.id, context: context, course: pack.id,
            confirmedCourseIdentity: Data(repeating: 9, count: 16)); XCTFail("Unconfirmed course accepted") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
    }
    func testValidatedConversionPreservesUndoFlagsAndPackedState() throws {
        let bytes = record() + record(uid: 2, op: 4) + record(uid: 2, op: 8, arg: 0) + record(op: 16, arg: 5)
        let journal = try LegacyTintaJournal(courseIdentity: course, bytes: bytes)
        var reviewed = try ScheduledItem(uid: 1).reviewed(grade: 3, day: 10, configuration: SchedulerConfiguration()).bytes
        reviewed[14] = 5
        let expected = [UInt32(1): try ScheduledItem(decoding: reviewed), UInt32(2): try ScheduledItem(uid: 2)]
        let plan = try journal.migrationPlan(context: context(), expectedItems: expected)
        XCTAssertEqual(plan.originalHash, journal.originalHash); XCTAssertEqual(plan.mutations.count, 5)
        let undo = try TintaBody(mutation: plan.mutations[2])
        XCTAssertEqual(undo.value, .undo(plan.mutations[1].event.identity))
        XCTAssertEqual(plan.snapshot.items[try TintaSubject(course: course, uid: 2)], try ScheduledItem(uid: 2))
        XCTAssertEqual(plan.snapshot.studyTotals[course]?[10]?.newItems, 1)
        for mutation in plan.mutations {
            XCTAssertEqual(mutation.event.clockQuality, .unknown); XCTAssertEqual(mutation.event.timestamp, 0)
            _ = try TintaBody(mutation: mutation)
        }
        XCTAssertEqual(plan.mutations.map { $0.event.identity.sequence }, [1, 2, 3, 4, 5])
    }
    func testConfigurationAndDerivedStateMismatchRejectPlan() throws {
        let journal = try LegacyTintaJournal(courseIdentity: course, bytes: record(op: 4))
        let expected = try ScheduledItem(uid: 1).reviewed(grade: 4, day: 10, configuration: SchedulerConfiguration())
        XCTAssertThrowsError(try journal.migrationPlan(context: context(retention: 8500), expectedItems: [1: expected])) {
            XCTAssertEqual($0 as? LegacyMigrationError, .stateMismatch)
        }
        XCTAssertThrowsError(try journal.migrationPlan(context: context(), expectedItems: [:]))
        XCTAssertThrowsError(try journal.migrationPlan(context: context(), expectedItems: [2: expected])) {
            XCTAssertEqual($0 as? LegacyMigrationError, .invalidExpectedState)
        }
    }
    func testUnrepresentableLeechControlIsPreservedAsConflict() throws {
        let journal = try LegacyTintaJournal(courseIdentity: course, bytes: record(op: 16, arg: 2))
        XCTAssertThrowsError(try journal.migrationPlan(context: context(), expectedItems: [:])) {
            XCTAssertEqual($0 as? LegacyMigrationError, .unrepresentableFlags(0))
        }
        XCTAssertEqual(journal.originalBytes, record(op: 16, arg: 2))
    }
    func testPureUndoKeepsFreshRecordAndEmptyMigrationIsValid() throws {
        let journal = try LegacyTintaJournal(courseIdentity: course, bytes: record() + record(op: 8, arg: 0))
        let plan = try journal.migrationPlan(context: context(), expectedItems: [1: ScheduledItem(uid: 1)])
        XCTAssertEqual(plan.snapshot.items.count, 1)
        XCTAssertTrue(plan.snapshot.studyTotals.isEmpty)
        let empty = try LegacyTintaJournal(courseIdentity: course, bytes: Data())
        XCTAssertTrue(try empty.migrationPlan(context: context(), expectedItems: [:]).mutations.isEmpty)
    }
}

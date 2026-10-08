import Foundation
import XCTest
@testable import CompanionKit

final class LegacyBackupTests: XCTestCase, @unchecked Sendable {
    func testBackupRestartAndChangedSourceRejection() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let vaultRoot = root.appendingPathComponent("vault")
        let vault = try ContentVault(root: vaultRoot)
        var sources: [LegacyBackupRole: URL] = [:]
        var files: [LegacyBackupFile] = []; files.reserveCapacity(3)
        for role in [LegacyBackupRole.reviews, .items, .profile] {
            let source = root.appendingPathComponent(role.rawValue)
            try Data(role.rawValue.utf8).write(to: source)
            sources[role] = source
            let object = try await vault.importFile(source)
            files.append(LegacyBackupFile(role: role, id: object.id, length: object.length))
        }
        let manifest = try LegacyBackupManifest(reader: Data(repeating: 1, count: 16),
            generation: Data(repeating: 2, count: 16), course: Data(repeating: 3, count: 16), files: files)
        let receipt = try await vault.preserveLegacyBackup(manifest, sources: sources)
        let duplicate = try await vault.preserveLegacyBackup(manifest, sources: sources)
        XCTAssertEqual(receipt.id, duplicate.id)
        let restarted = try ContentVault(root: vaultRoot)
        let recovered = try await restarted.verifiedLegacyBackup(receipt.id)
        XCTAssertEqual(recovered, manifest)
        let databaseURL = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: databaseURL, random: { Data(repeating: 9, count: $0) })
        let registered = try await library.preserveLegacyBackup(manifest, sources: sources, vault: vault)
        XCTAssertEqual(registered, receipt.id)
        try await library.registerLegacyBackup(receipt.id, vault: vault)
        let reopened = try LibraryStore(url: databaseURL)
        let receipts = try await reopened.legacyBackupIDs(reader: manifest.reader, generation: manifest.generation, course: manifest.course)
        XCTAssertEqual(receipts, [receipt.id])
        let differentGeneration = try await reopened.legacyBackupIDs(reader: manifest.reader,
            generation: Data(repeating: 4, count: 16), course: manifest.course)
        XCTAssertEqual(differentGeneration, [])
        let origin = Data(repeating: 7, count: 16), resource = Data(repeating: 8, count: 32)
        let configuration = try SchedulerConfiguration()
        let migration = try await library.reserveLegacyMigration(backup: receipt.id, origin: origin,
            resource: resource, configuration: configuration)
        let restoredMigration = try await reopened.reserveLegacyMigration(backup: receipt.id, origin: origin,
            resource: resource, configuration: configuration)
        XCTAssertEqual(restoredMigration.epoch, migration.epoch)
        XCTAssertEqual(restoredMigration.generation, manifest.generation)
        let conflictingManifest = try LegacyBackupManifest(reader: manifest.reader, generation: manifest.generation,
            course: Data(repeating: 4, count: 16), files: files)
        let conflictingBackup = try await library.preserveLegacyBackup(conflictingManifest, sources: sources, vault: vault)
        let unassociated = try ContentID(resource.map { String(format: "%02x", $0) }.joined())
        let absentAssociation = try await library.courseIdentity(unassociated)
        XCTAssertNil(absentAssociation)
        do {
            _ = try await reopened.reserveLegacyMigration(backup: conflictingBackup, origin: Data(repeating: 6, count: 16),
                resource: resource, configuration: configuration)
            XCTFail("unassociated pack must not reserve conflicting course namespaces")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let retainedMigration = try await reopened.reserveLegacyMigration(backup: receipt.id, origin: origin,
            resource: resource, configuration: configuration)
        XCTAssertEqual(retainedMigration.epoch, restoredMigration.epoch)
        XCTAssertEqual(retainedMigration.resource, restoredMigration.resource)
        XCTAssertEqual(retainedMigration.generation, restoredMigration.generation)
        do { _ = try await reopened.reserveLegacyMigration(backup: receipt.id, origin: origin,
            resource: resource, configuration: SchedulerConfiguration(retentionBasisPoints: 8500)); XCTFail("Changed configuration accepted") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let missing = try ContentID(String(repeating: "f", count: 64))
        do { _ = try await reopened.reserveLegacyMigration(backup: missing, origin: origin,
            resource: resource, configuration: configuration); XCTFail("Missing backup accepted") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        try Data("changed".utf8).write(to: sources[.reviews]!)
        do { _ = try await vault.preserveLegacyBackup(manifest, sources: sources); XCTFail("Changed snapshot accepted") }
        catch { XCTAssertEqual(error as? VaultError, .integrity) }
        do { _ = try await library.preserveLegacyBackup(manifest, sources: sources, vault: vault); XCTFail("Changed backup registered") }
        catch { XCTAssertEqual(error as? VaultError, .integrity) }
        let retained = try await restarted.verifiedLegacyBackup(receipt.id)
        XCTAssertEqual(retained, manifest)
        let review = try await restarted.verifiedObject(files.first { $0.role == .reviews }!.id)
        try Data("broken".utf8).write(to: review.url)
        do { try await reopened.registerLegacyBackup(receipt.id, vault: restarted); XCTFail("Corrupt backup registered") }
        catch { XCTAssertEqual(error as? VaultError, .integrity) }
        do { _ = try await restarted.verifiedLegacyBackup(receipt.id); XCTFail("Corrupt original accepted") }
        catch { XCTAssertEqual(error as? VaultError, .integrity) }
    }
    func testMissingAndDuplicateRolesRejected() throws {
        let id = try ContentID(String(repeating: "a", count: 64))
        let file = LegacyBackupFile(role: .reviews, id: id, length: 0)
        XCTAssertThrowsError(try LegacyBackupManifest(reader: Data(repeating: 1, count: 16),
            generation: Data(repeating: 2, count: 16), course: Data(repeating: 3, count: 16), files: [file, file]))
    }
}

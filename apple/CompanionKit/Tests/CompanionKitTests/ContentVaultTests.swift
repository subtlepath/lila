import Foundation
import XCTest
@testable import CompanionKit

final class ContentVaultTests: XCTestCase, @unchecked Sendable {
    private func directory() throws -> URL {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        return root
    }
    func testKnownHashDeduplicationAndRestart() async throws {
        let root = try directory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("source.epub")
        try Data("abc".utf8).write(to: source)
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let first = try await vault.importFile(source)
        XCTAssertEqual(first.id.hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        XCTAssertEqual(first.length, 3)
        let renamed = root.appendingPathComponent("renamed.epub")
        try FileManager.default.moveItem(at: source, to: renamed)
        let duplicate = try await vault.importFile(renamed)
        XCTAssertEqual(first, duplicate)
        let restarted = try ContentVault(root: root.appendingPathComponent("vault"))
        let recovered = try await restarted.verifiedObject(first.id)
        XCTAssertEqual(first, recovered)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: root.appendingPathComponent("vault/staging").path), [])
    }
    func testLargeCopyPreservesBytesAndDifferentEditionsRemainDistinct() async throws {
        let root = try directory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("source")
        var bytes = Data(repeating: 42, count: 131079)
        try bytes.write(to: source)
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let first = try await vault.importFile(source)
        XCTAssertEqual(first.length, UInt64(bytes.count))
        XCTAssertEqual(try Data(contentsOf: first.url), bytes)
        bytes[0] = 43
        try bytes.write(to: source)
        let second = try await vault.importFile(source)
        XCTAssertNotEqual(first.id, second.id)
        XCTAssertEqual(try Data(contentsOf: first.url).first, 42)
    }
    func testCorruptExistingObjectIsRejectedWithoutReplacement() async throws {
        let root = try directory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("source")
        try Data("abc".utf8).write(to: source)
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let stored = try await vault.importFile(source)
        try Data("broken".utf8).write(to: stored.url)
        do { _ = try await vault.verifiedObject(stored.id); XCTFail("Corruption accepted") }
        catch { XCTAssertEqual(error as? VaultError, .integrity) }
        do { _ = try await vault.importFile(source); XCTFail("Corrupt object overwritten") }
        catch { XCTAssertEqual(error as? VaultError, .integrity) }
        XCTAssertEqual(try Data(contentsOf: stored.url), Data("broken".utf8))
    }
    func testCancelledImportDoesNotPublishPartialContent() async throws {
        let root = try directory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("source")
        try Data("abc".utf8).write(to: source)
        let vaultRoot = root.appendingPathComponent("vault")
        let vault = try ContentVault(root: vaultRoot)
        let task = Task {
            withUnsafeCurrentTask { $0?.cancel() }
            return try await vault.importFile(source)
        }
        do { _ = try await task.value; XCTFail("Cancelled import accepted") }
        catch { XCTAssertTrue(error is CancellationError) }
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: vaultRoot.appendingPathComponent("staging").path), [])
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: vaultRoot.appendingPathComponent("objects").path), [])
    }
    func testRecoveryReclaimsOwnedStagesAndPreservesObjectsAndUnknownFiles() async throws {
        let root = try directory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("source")
        try Data("abc".utf8).write(to: source)
        let vaultRoot = root.appendingPathComponent("vault")
        let first = try ContentVault(root: vaultRoot)
        let stored = try await first.importFile(source)
        let staging = vaultRoot.appendingPathComponent("staging")
        let stale = staging.appendingPathComponent(UUID().uuidString + ".partial")
        let unknown = staging.appendingPathComponent("unknown.partial")
        try Data("unfinished".utf8).write(to: stale)
        try Data("keep".utf8).write(to: unknown)
        let restarted = try ContentVault(root: vaultRoot)
        XCTAssertFalse(FileManager.default.fileExists(atPath: stale.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: unknown.path))
        let verified = try await restarted.verifiedObject(stored.id)
        XCTAssertEqual(verified, stored)
    }
    func testActiveWriterPreventsRecoveryAcrossVaultInstances() async throws {
        let root = try directory()
        defer { try? FileManager.default.removeItem(at: root) }
        let vaultRoot = root.appendingPathComponent("vault")
        let first = try ContentVault(root: vaultRoot)
        let active = try VaultLock(root: vaultRoot, exclusive: false, nonblocking: false)
        defer { active.release() }
        let stage = vaultRoot.appendingPathComponent("staging/" + UUID().uuidString + ".partial")
        try Data("active writer".utf8).write(to: stage)
        _ = try ContentVault(root: vaultRoot)
        let blocked = try await first.reclaimInterruptedImports()
        XCTAssertEqual(blocked, 0)
        XCTAssertTrue(FileManager.default.fileExists(atPath: stage.path))
        active.release()
        let removed = try await first.reclaimInterruptedImports()
        XCTAssertEqual(removed, 1)
        XCTAssertFalse(FileManager.default.fileExists(atPath: stage.path))
    }

}

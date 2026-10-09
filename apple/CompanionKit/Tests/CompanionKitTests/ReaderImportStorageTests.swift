import Foundation
import XCTest
@testable import CompanionKit

final class ReaderImportStorageTests: XCTestCase, @unchecked Sendable {
    private func fixture() throws -> (URL, ReaderImportStorage, ReaderImportJob) {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let storage = try ReaderImportStorage(root: root.appendingPathComponent("reader-imports"))
        let manifest = try ContentManifest(content: ContentID("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
                                           kind: .epub, length: 3, formatVersion: 1, logicalIdentity: Data(count: 16))
        let job = ReaderImportJob(id: UUID(), reader: Data(repeating: 1, count: 16), generation: Data(repeating: 2, count: 16),
                                  installation: Data(repeating: 3, count: 16), manifest: manifest, acknowledgedOffset: 0, phase: .downloading)
        return (root, storage, job)
    }
    private func copy(_ job: ReaderImportJob, offset: UInt64, phase: ReaderImportJobPhase) -> ReaderImportJob {
        ReaderImportJob(id: job.id, reader: job.reader, generation: job.generation, installation: job.installation,
                        manifest: job.manifest, acknowledgedOffset: offset, phase: phase)
    }
    private func rejected(_ operation: () async throws -> Void) async {
        do { try await operation(); XCTFail("Invalid storage operation accepted") } catch {}
    }
    func testRestartPreservesAcknowledgedBytesAndVaultRecoveryDoesNotReclaimDownload() async throws {
        let (root, storage, job) = try fixture(); defer { try? FileManager.default.removeItem(at: root) }
        let offset = try await storage.append(Data("ab".utf8), to: job); XCTAssertEqual(offset, 2)
        let resumed = copy(job, offset: 2, phase: .downloading)
        let reopened = try ReaderImportStorage(root: root.appendingPathComponent("reader-imports"))
        let prepared = try await reopened.prepare(resumed); XCTAssertEqual(prepared, 2)
        _ = try ContentVault(root: root)
        let end = try await reopened.append(Data("c".utf8), to: resumed); XCTAssertEqual(end, 3)
        let source = try await reopened.completedSource(copy(job, offset: 3, phase: .verifying))
        XCTAssertEqual(try Data(contentsOf: source), Data("abc".utf8))
        let vault = try ContentVault(root: root)
        let (object, count) = try await vault.importValidatedFile(source, expectedID: job.manifest.content,
            expectedLength: job.manifest.length) { url in try Data(contentsOf: url).count }
        XCTAssertEqual(object.id, job.manifest.content); XCTAssertEqual(count, 3)
        try await reopened.discard(copy(job, offset: 3, phase: .completed))
        let retained = try await vault.verifiedObject(object.id); XCTAssertEqual(retained, object)
    }
    func testUnacknowledgedTailIsTrimmedAndMissingAcknowledgedBytesRefuseResume() async throws {
        let (root, storage, job) = try fixture(); defer { try? FileManager.default.removeItem(at: root) }
        _ = try await storage.append(Data("abc".utf8), to: job)
        let reopened = try ReaderImportStorage(root: root.appendingPathComponent("reader-imports"))
        let durable = copy(job, offset: 1, phase: .paused)
        _ = try await reopened.prepare(durable)
        let path = root.appendingPathComponent("reader-imports/\(job.id.uuidString)/download")
        XCTAssertEqual(try Data(contentsOf: path), Data("a".utf8))
        await rejected { _ = try await reopened.prepare(self.copy(job, offset: 2, phase: .paused)) }
        XCTAssertEqual(try Data(contentsOf: path), Data("a".utf8))
        _ = try await reopened.prepare(copy(job, offset: 0, phase: .queued))
        XCTAssertEqual(try Data(contentsOf: path), Data())
    }
    func testForeignBindingCannotTruncateAppendOrDiscardExistingDownload() async throws {
        let (root, storage, job) = try fixture(); defer { try? FileManager.default.removeItem(at: root) }
        _ = try await storage.append(Data("ab".utf8), to: job)
        let foreign = ReaderImportJob(id: job.id, reader: job.reader, generation: Data(repeating: 4, count: 16),
            installation: job.installation, manifest: job.manifest, acknowledgedOffset: 0, phase: .downloading)
        await rejected { _ = try await storage.prepare(foreign) }
        await rejected { _ = try await storage.append(Data("c".utf8), to: foreign) }
        await rejected { try await storage.discard(self.copy(foreign, offset: 0, phase: .aborted)) }
        let path = root.appendingPathComponent("reader-imports/\(job.id.uuidString)/download")
        XCTAssertEqual(try Data(contentsOf: path), Data("ab".utf8))
    }
    func testInvalidPhasesAndChunkBoundsDoNotMutateFile() async throws {
        let (root, storage, job) = try fixture(); defer { try? FileManager.default.removeItem(at: root) }
        _ = try await storage.prepare(job)
        await rejected { _ = try await storage.append(Data(), to: job) }
        await rejected { _ = try await storage.append(Data(repeating: 1, count: 962), to: job) }
        await rejected { _ = try await storage.append(Data("abcd".utf8), to: job) }
        await rejected { _ = try await storage.append(Data("a".utf8), to: self.copy(job, offset: 0, phase: .paused)) }
        await rejected { _ = try await storage.completedSource(job) }
        await rejected { try await storage.discard(job) }
        let path = root.appendingPathComponent("reader-imports/\(job.id.uuidString)/download")
        XCTAssertEqual(try Data(contentsOf: path), Data())
    }
    func testSymlinkDownloadIsRefusedWithoutTouchingTarget() async throws {
        let (root, storage, job) = try fixture(); defer { try? FileManager.default.removeItem(at: root) }
        _ = try await storage.prepare(job)
        let target = root.appendingPathComponent("unrelated")
        try Data("keep".utf8).write(to: target)
        let download = root.appendingPathComponent("reader-imports/\(job.id.uuidString)/download")
        try FileManager.default.removeItem(at: download)
        try FileManager.default.createSymbolicLink(at: download, withDestinationURL: target)
        await rejected { _ = try await storage.prepare(job) }
        XCTAssertEqual(try Data(contentsOf: target), Data("keep".utf8))
    }
    func testCorruptBindingFailsClosedAndTerminalCleanupIsIdempotent() async throws {
        let (root, storage, job) = try fixture(); defer { try? FileManager.default.removeItem(at: root) }
        _ = try await storage.prepare(job)
        let binding = root.appendingPathComponent("reader-imports/\(job.id.uuidString)/binding")
        let original = try Data(contentsOf: binding)
        try Data(original.prefix(40)).write(to: binding)
        await rejected { _ = try await storage.prepare(job) }
        try original.write(to: binding)
        let aborted = copy(job, offset: 0, phase: .aborted)
        try await storage.discard(aborted); try await storage.discard(aborted)
        XCTAssertFalse(FileManager.default.fileExists(atPath: binding.path))
    }
    func testInterruptedBindingStageDoesNotPreventNewAtomicBinding() async throws {
        let (root, storage, job) = try fixture(); defer { try? FileManager.default.removeItem(at: root) }
        let directory = root.appendingPathComponent("reader-imports/\(job.id.uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        let orphan = directory.appendingPathComponent(UUID().uuidString + ".partial")
        try Data([0x4c, 0x49]).write(to: orphan)
        let offset = try await storage.append(Data("abc".utf8), to: job); XCTAssertEqual(offset, 3)
        let reopened = try ReaderImportStorage(root: root.appendingPathComponent("reader-imports"))
        let source = try await reopened.completedSource(copy(job, offset: 3, phase: .verifying))
        XCTAssertEqual(try Data(contentsOf: source), Data("abc".utf8))
        XCTAssertEqual(try Data(contentsOf: orphan), Data([0x4c, 0x49]))
    }

    func testCleanupResumesAfterPayloadDeletionAndAfterBindingDeletion() async throws {
        for removeBinding in [false, true] {
            let (root, storage, job) = try fixture()
            defer { try? FileManager.default.removeItem(at: root) }
            _ = try await storage.append(Data("ab".utf8), to: job)
            let directory = root.appendingPathComponent("reader-imports/\(job.id.uuidString)")
            try FileManager.default.removeItem(at: directory.appendingPathComponent("download"))
            if removeBinding { try FileManager.default.removeItem(at: directory.appendingPathComponent("binding")) }
            let reopened = try ReaderImportStorage(root: root.appendingPathComponent("reader-imports"))
            try await reopened.discard(copy(job, offset: 2, phase: .aborted))
            try await reopened.discard(copy(job, offset: 2, phase: .aborted))
            XCTAssertFalse(FileManager.default.fileExists(atPath: directory.path))
        }
    }

    func testCleanupRefusesPayloadWithoutOwnershipBinding() async throws {
        let (root, storage, job) = try fixture()
        defer { try? FileManager.default.removeItem(at: root) }
        _ = try await storage.append(Data("ab".utf8), to: job)
        let directory = root.appendingPathComponent("reader-imports/\(job.id.uuidString)")
        try FileManager.default.removeItem(at: directory.appendingPathComponent("binding"))
        let reopened = try ReaderImportStorage(root: root.appendingPathComponent("reader-imports"))
        await rejected { try await reopened.discard(self.copy(job, offset: 2, phase: .aborted)) }
        XCTAssertEqual(try Data(contentsOf: directory.appendingPathComponent("download")), Data("ab".utf8))
    }

}

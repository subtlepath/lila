import Foundation
import CSQLite
import XCTest
@testable import CompanionKit

final class ReaderImportJobTests: XCTestCase, @unchecked Sendable {
    private func setup() throws -> (URL, LibraryStore, ContentManifest, ReaderInventory, Data) {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let manifest = try ContentManifest(content: ContentID(String(repeating: "a", count: 64)), kind: .epub,
                                           length: 123, formatVersion: 1, logicalIdentity: Data(count: 16))
        let inventory = try ReaderInventory(reader: Data(repeating: 1, count: 16), generation: Data(repeating: 2, count: 16),
                                             contents: [manifest], complete: true)
        return (root, store, manifest, inventory, Data(repeating: 3, count: 16))
    }
    private func savedJob(_ store: LibraryStore, _ id: UUID) async throws -> ReaderImportJob {
        let saved = try await store.readerImportJob(id)
        return try XCTUnwrap(saved)
    }
    private func rejected(_ operation: () async throws -> Void) async {
        do { try await operation(); XCTFail("Invalid operation accepted") } catch {}
    }
    func testSchema38MigrationPreservesLibraryAndCreatesImportQueue() async throws {
        let (root, store, manifest, inventory, owner) = try setup()
        defer { try? FileManager.default.removeItem(at: root) }
        try await store.put(LibraryContent(id: manifest.content, kind: .epub, length: manifest.length,
                                           title: "Existing book", originalFilename: "existing.epub"))
        var handle: OpaquePointer?
        guard sqlite3_open(root.appendingPathComponent("library.sqlite").path, &handle) == SQLITE_OK else {
            if let handle { sqlite3_close(handle) }; XCTFail("Cannot open migration fixture"); return
        }
        let status = sqlite3_exec(handle, "DROP TABLE reader_import_filenames; DROP TABLE reader_import_jobs; PRAGMA user_version=38;", nil, nil, nil)
        sqlite3_close(handle)
        guard status == SQLITE_OK else { XCTFail("Cannot prepare schema-38 fixture"); return }
        let migrated = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let pending = try await migrated.pendingReaderImports(); XCTAssertTrue(pending.isEmpty)
        let contents = try await migrated.libraryContentIDs(); XCTAssertEqual(contents, [manifest.content])
        let job = try await migrated.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        XCTAssertEqual(job.phase, .queued); XCTAssertEqual(job.acknowledgedOffset, 0)
    }
    func testReaderOnlyQueueIsDurableAndIdempotentWithoutPublishingMetadata() async throws {
        let (root, store, manifest, inventory, owner) = try setup()
        defer { try? FileManager.default.removeItem(at: root) }
        let job = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        let duplicate = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        XCTAssertEqual(duplicate, job)
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let pending = try await reopened.pendingReaderImports()
        XCTAssertEqual(pending, [job])
        let contents = try await reopened.libraryContentIDs()
        let selections = try await reopened.readerSelections(reader: inventory.reader)
        XCTAssertTrue(contents.isEmpty); XCTAssertTrue(selections.isEmpty)
    }
    func testCheckpointAndPausedRestartPreserveOwnership() async throws {
        let (root, store, manifest, inventory, owner) = try setup()
        defer { try? FileManager.default.removeItem(at: root) }
        let job = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        try await store.checkpointReaderImport(job.id, offset: 0, phase: .downloading)
        try await store.checkpointReaderImport(job.id, offset: 17, phase: .downloading)
        try await store.checkpointReaderImport(job.id, offset: 17, phase: .paused)
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let resumed = try await savedJob(reopened, job.id)
        XCTAssertEqual(resumed.acknowledgedOffset, 17); XCTAssertEqual(resumed.phase, .paused)
        XCTAssertEqual(resumed.manifest, manifest); XCTAssertEqual(resumed.installation, owner)
        try await reopened.restartReaderImport(job.id)
        let restarted = try await savedJob(reopened, job.id)
        XCTAssertEqual(restarted.acknowledgedOffset, 0); XCTAssertEqual(restarted.phase, .queued)
        XCTAssertEqual(restarted.reader, job.reader); XCTAssertEqual(restarted.generation, job.generation)
    }
    func testInvalidOffsetsAndPublicationCannotAdvanceJob() async throws {
        let (root, store, manifest, inventory, owner) = try setup()
        defer { try? FileManager.default.removeItem(at: root) }
        let job = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        await rejected { try await store.checkpointReaderImport(job.id, offset: 10, phase: .downloading) }
        try await store.checkpointReaderImport(job.id, offset: 0, phase: .downloading)
        try await store.checkpointReaderImport(job.id, offset: 10, phase: .downloading)
        await rejected { try await store.checkpointReaderImport(job.id, offset: 9, phase: .downloading) }
        await rejected { try await store.checkpointReaderImport(job.id, offset: UInt64.max, phase: .downloading) }
        await rejected { try await store.checkpointReaderImport(job.id, offset: 10, phase: .verifying) }
        await rejected { try await store.checkpointReaderImport(job.id, offset: 123, phase: .verifying) }
        await rejected { try await store.checkpointReaderImport(job.id, offset: 10, phase: .completed) }
        await rejected { try await store.restartReaderImport(job.id) }
        let unchanged = try await savedJob(store, job.id)
        XCTAssertEqual(unchanged.acknowledgedOffset, 10); XCTAssertEqual(unchanged.phase, .downloading)
        try await store.checkpointReaderImport(job.id, offset: 123, phase: .downloading)
        try await store.checkpointReaderImport(job.id, offset: 123, phase: .verifying)
        await rejected { try await store.checkpointReaderImport(job.id, offset: 123, phase: .completed) }
        let verifying = try await savedJob(store, job.id)
        XCTAssertEqual(verifying.phase, .verifying)
    }
    func testForeignBindingsAndIncompleteInventoryCannotReplaceIntent() async throws {
        let (root, store, manifest, inventory, owner) = try setup()
        defer { try? FileManager.default.removeItem(at: root) }
        let job = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        let incomplete = try ReaderInventory(reader: inventory.reader, generation: inventory.generation,
                                              contents: [manifest], complete: false)
        await rejected { _ = try await store.enqueueReaderImport(manifest: manifest, inventory: incomplete, installation: owner) }
        await rejected { _ = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory,
                                                                installation: Data(repeating: 4, count: 16)) }
        let otherCard = try ReaderInventory(reader: inventory.reader, generation: Data(repeating: 5, count: 16),
                                            contents: [manifest], complete: true)
        await rejected { _ = try await store.enqueueReaderImport(manifest: manifest, inventory: otherCard, installation: owner, id: job.id) }
        let saved = try await store.readerImportJob(job.id)
        XCTAssertEqual(saved, job)
        let second = try await store.enqueueReaderImport(manifest: manifest, inventory: otherCard, installation: owner)
        XCTAssertNotEqual(second.id, job.id)
        let pending = try await store.pendingReaderImports()
        XCTAssertEqual(pending.count, 2)
    }
    func testRemovalAndTransfersCannotTakeImportIdentityOrDeletePendingSource() async throws {
        let (root, store, manifest, inventory, owner) = try setup()
        defer { try? FileManager.default.removeItem(at: root) }
        let job = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        try await store.put(LibraryContent(id: manifest.content, kind: .epub, length: manifest.length,
                                           title: "Book", originalFilename: "book.epub"))
        try await store.setReaderSelection(reader: inventory.reader, content: manifest.content, selected: true)
        await rejected { _ = try await store.enqueue(content: manifest.content, reader: inventory.reader,
                                                      storageGeneration: inventory.generation, installation: owner, transaction: job.id) }
        await rejected { _ = try await store.queueRemoval(manifest: manifest, inventory: inventory, installation: owner, transaction: job.id) }
        await rejected { _ = try await store.queueRemoval(manifest: manifest, inventory: inventory, installation: owner) }
        let selected = try await store.isReaderContentSelected(reader: inventory.reader, content: manifest.content)
        XCTAssertTrue(selected)
        let saved = try await store.readerImportJob(job.id); XCTAssertEqual(saved, job)
        try await store.abortReaderImport(job.id)
        let removal = try await store.queueRemoval(manifest: manifest, inventory: inventory, installation: owner)
        await rejected { _ = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner) }
        let removals = try await store.pendingRemovalJobs(); XCTAssertEqual(removals, [removal])
        let pending = try await store.pendingReaderImports(); XCTAssertTrue(pending.isEmpty)
    }
    func testAbortRetainsOffsetAndAllowsSeparateNewIntent() async throws {
        let (root, store, manifest, inventory, owner) = try setup()
        defer { try? FileManager.default.removeItem(at: root) }
        let job = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        try await store.checkpointReaderImport(job.id, offset: 0, phase: .downloading)
        try await store.checkpointReaderImport(job.id, offset: 5, phase: .downloading)
        try await store.abortReaderImport(job.id); try await store.abortReaderImport(job.id)
        await rejected { try await store.checkpointReaderImport(job.id, offset: 5, phase: .downloading) }
        await rejected { try await store.restartReaderImport(job.id) }
        let terminal = try await savedJob(store, job.id)
        XCTAssertEqual(terminal.phase, .aborted); XCTAssertEqual(terminal.acknowledgedOffset, 5)
        let pending = try await store.pendingReaderImports(); XCTAssertTrue(pending.isEmpty)
        let replacement = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        XCTAssertNotEqual(replacement.id, job.id)
        XCTAssertEqual(replacement.acknowledgedOffset, 0)
    }
    func testGlobalDeletionAbortsDownloadAndRestoreDoesNotResumeOldIntent() async throws {
        let (root, store, manifest, inventory, owner) = try setup()
        defer { try? FileManager.default.removeItem(at: root) }
        try await store.put(LibraryContent(id: manifest.content, kind: .epub, length: manifest.length,
                                           title: "Book", originalFilename: "book.epub"))
        let job = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        try await store.checkpointReaderImport(job.id, offset: 0, phase: .downloading)
        try await store.checkpointReaderImport(job.id, offset: 5, phase: .downloading)
        let deleted = try await store.deleteLibraryContent(manifest.content); XCTAssertTrue(deleted)
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let aborted = try await savedJob(reopened, job.id)
        XCTAssertEqual(aborted.phase, .aborted); XCTAssertEqual(aborted.acknowledgedOffset, 5)
        await rejected { _ = try await reopened.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner) }
        try await reopened.restoreLibraryContent(manifest.content)
        await rejected { try await reopened.restartReaderImport(job.id) }
        let newJob = try await reopened.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        XCTAssertNotEqual(newJob.id, job.id); XCTAssertEqual(newJob.acknowledgedOffset, 0)
    }
    func testSyncedDeletionCancelsReaderOnlyImportBeforeMetadataArrives() async throws {
        let (root, store, manifest, inventory, owner) = try setup()
        defer { try? FileManager.default.removeItem(at: root) }
        let job = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        try await store.checkpointReaderImport(job.id, offset: 0, phase: .downloading)
        try await store.checkpointReaderImport(job.id, offset: manifest.length, phase: .downloading)
        try await store.checkpointReaderImport(job.id, offset: manifest.length, phase: .verifying)
        let deletion = try LibraryVisibilityChange(origin: Data(repeating: 8, count: 16), content: manifest.content,
                                                   removed: true, ancestors: [])
        _ = try await store.importLibraryVisibilityChanges([deletion])
        let aborted = try await savedJob(store, job.id); XCTAssertEqual(aborted.phase, .aborted)
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        await rejected { _ = try await reopened.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner) }
        _ = try await reopened.importLibraryVisibilityChanges([deletion])
        let restore = try LibraryVisibilityChange(origin: owner, content: manifest.content, removed: false, ancestors: [deletion.id])
        _ = try await reopened.importLibraryVisibilityChanges([restore])
        let newJob = try await reopened.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        XCTAssertNotEqual(newJob.id, job.id)
        let oldJob = try await savedJob(reopened, job.id); XCTAssertEqual(oldJob.phase, .aborted)
    }

    func testReaderFilenameIsDurableImmutableAndBoundToManifest() async throws {
        let (root, store, manifest, inventory, owner) = try setup()
        defer { try? FileManager.default.removeItem(at: root) }
        let job = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        let request = try ReaderContentMetadataRequest(generation: inventory.generation, manifest: manifest)
        func reply(_ request: ReaderContentMetadataRequest, name: String) throws -> ReaderContentMetadataReply {
            let nameBytes = Data(name.utf8)
            var bytes = Data([0x4c, 0x43, 0x4e, 1, 0])
            bytes.append(request.generation); bytes.append(request.manifest.encoded)
            bytes.append(UInt8(nameBytes.count)); bytes.append(nameBytes)
            return try ReaderContentMetadataReply(decoding: bytes, request: request)
        }
        let accepted = try reply(request, name: "読書.epub")
        let name = try await store.bindReaderImportFilename(job.id, request: request, reply: accepted)
        XCTAssertEqual(name, "読書.epub")
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let saved = try await reopened.readerImportFilename(job.id)
        XCTAssertEqual(saved, name)
        try await reopened.checkpointReaderImport(job.id, offset: 0, phase: .downloading)
        let repeated = try await reopened.bindReaderImportFilename(job.id, request: request, reply: accepted)
        XCTAssertEqual(repeated, name)
        await rejected { _ = try await reopened.bindReaderImportFilename(job.id, request: request,
                                                                         reply: reply(request, name: "changed.epub")) }
        let foreign = try ReaderContentMetadataRequest(generation: Data(repeating: 9, count: 16), manifest: manifest)
        await rejected { _ = try await reopened.bindReaderImportFilename(job.id, request: request,
                                                                         reply: reply(foreign, name: "読書.epub")) }
        try await reopened.checkpointReaderImport(job.id, offset: 0, phase: .paused)
        try await reopened.restartReaderImport(job.id)
        let restarted = try await reopened.readerImportFilename(job.id)
        XCTAssertEqual(restarted, name)
        try await reopened.abortReaderImport(job.id)
        await rejected { _ = try await reopened.bindReaderImportFilename(job.id, request: request, reply: accepted) }
    }

    func testSchema39MigrationPreservesPendingImportWithoutInventingFilename() async throws {
        let (root, store, manifest, inventory, owner) = try setup()
        defer { try? FileManager.default.removeItem(at: root) }
        let job = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
        var handle: OpaquePointer?
        guard sqlite3_open(root.appendingPathComponent("library.sqlite").path, &handle) == SQLITE_OK else {
            if let handle { sqlite3_close(handle) }; XCTFail("Cannot open migration fixture"); return
        }
        let status = sqlite3_exec(handle, "DROP TABLE reader_import_filenames; PRAGMA user_version=39;", nil, nil, nil)
        sqlite3_close(handle)
        guard status == SQLITE_OK else { XCTFail("Cannot prepare schema-39 fixture"); return }
        let migrated = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let saved = try await migrated.readerImportJob(job.id)
        XCTAssertEqual(saved, job)
        let filename = try await migrated.readerImportFilename(job.id)
        XCTAssertNil(filename)
    }

    func testMigratedInterruptedJobsCanAcceptFirstFilenameWithoutLosingOffset() async throws {
        for phase in [ReaderImportJobPhase.downloading, .paused, .verifying] {
            let (root, store, manifest, inventory, owner) = try setup()
            defer { try? FileManager.default.removeItem(at: root) }
            let job = try await store.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: owner)
            try await store.checkpointReaderImport(job.id, offset: 0, phase: .downloading)
            let offset: UInt64 = phase == .verifying ? manifest.length : 17
            try await store.checkpointReaderImport(job.id, offset: offset, phase: .downloading)
            if phase != .downloading { try await store.checkpointReaderImport(job.id, offset: offset, phase: phase) }
            var handle: OpaquePointer?
            guard sqlite3_open(root.appendingPathComponent("library.sqlite").path, &handle) == SQLITE_OK else {
                if let handle { sqlite3_close(handle) }; XCTFail("Cannot open migration fixture"); return
            }
            let status = sqlite3_exec(handle, "DROP TABLE reader_import_filenames; PRAGMA user_version=39;", nil, nil, nil)
            sqlite3_close(handle)
            guard status == SQLITE_OK else { XCTFail("Cannot prepare schema-39 fixture"); return }
            let migrated = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
            let request = try ReaderContentMetadataRequest(generation: inventory.generation, manifest: manifest)
            func reply(_ name: String) throws -> ReaderContentMetadataReply {
                let nameBytes = Data(name.utf8)
                var bytes = Data([0x4c, 0x43, 0x4e, 1, 0])
                bytes.append(request.generation); bytes.append(manifest.encoded)
                bytes.append(UInt8(nameBytes.count)); bytes.append(nameBytes)
                return try ReaderContentMetadataReply(decoding: bytes, request: request)
            }
            let bound = try await migrated.bindReaderImportFilename(job.id, request: request, reply: reply("Book.epub"))
            XCTAssertEqual(bound, "Book.epub")
            let saved = try await migrated.readerImportJob(job.id)
            XCTAssertEqual(saved?.phase, phase); XCTAssertEqual(saved?.acknowledgedOffset, offset)
            await rejected { _ = try await migrated.bindReaderImportFilename(job.id, request: request, reply: reply("Changed.epub")) }
        }
    }

}

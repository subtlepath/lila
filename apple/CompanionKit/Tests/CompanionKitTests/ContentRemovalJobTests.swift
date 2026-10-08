import Foundation
import XCTest
@testable import CompanionKit

final class ContentRemovalJobTests: XCTestCase, @unchecked Sendable {
    func testTransactionIdentityCannotOwnTransferAndRemoval() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let content = try ContentID(String(repeating: "b", count: 64))
        try await store.put(LibraryContent(id: content, kind: .epub, length: 123, title: "Book", originalFilename: "book.epub"))
        let reader = Data(repeating: 1, count: 16), generation = Data(repeating: 2, count: 16)
        let owner = Data(repeating: 3, count: 16)
        let manifest = try ContentManifest(content: content, kind: .epub, length: 123, formatVersion: 1, logicalIdentity: Data(count: 16))
        let inventory = try ReaderInventory(reader: reader, generation: generation, contents: [manifest], complete: true)
        let removal = try await store.queueRemoval(manifest: manifest, inventory: inventory, installation: owner)
        do {
            _ = try await store.enqueue(content: content, reader: reader, storageGeneration: generation,
                                        installation: owner, transaction: removal.id)
            XCTFail("Removal transaction must not become a transfer")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        let transfers = try await store.pendingJobs()
        XCTAssertTrue(transfers.isEmpty)
        try await store.setReaderSelection(reader: reader, content: content, selected: true)
        let second = try ContentID(String(repeating: "c", count: 64))
        try await store.put(LibraryContent(id: second, kind: .epub, length: 123, title: "Other", originalFilename: "other.epub"))
        let transfer = try await store.enqueue(content: second, reader: reader, storageGeneration: generation, installation: owner)
        do {
            _ = try await store.queueRemoval(manifest: manifest, inventory: inventory, installation: owner, transaction: transfer.id)
            XCTFail("Transfer transaction must not become a removal")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        let secondManifest = try ContentManifest(content: second, kind: .epub, length: 123, formatVersion: 1, logicalIdentity: Data(count: 16))
        let secondInventory = try ReaderInventory(reader: reader, generation: generation, contents: [secondManifest], complete: true)
        try await store.setReaderSelection(reader: reader, content: second, selected: true)
        do {
            _ = try await store.queueRemoval(manifest: secondManifest, inventory: secondInventory, installation: owner)
            XCTFail("Unfinished transfer must be resolved before removal queueing")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        let secondSelected = try await store.isReaderContentSelected(reader: reader, content: second)
        XCTAssertTrue(secondSelected)
        let choices = try await store.readerSelections(reader: reader).filter { $0.content == content }
        XCTAssertEqual(choices, [ReaderContentSelection(content: content, selected: true)])
        let removals = try await store.pendingRemovalJobs()
        XCTAssertEqual(removals, [removal])
    }

    func testRemovalRetainsRequestAcrossRestartAndPreservesLibrary() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let store = try LibraryStore(url: url)
        let content = try ContentID(String(repeating: "a", count: 64))
        let book = LibraryContent(id: content, kind: .epub, length: 123, title: "Book", originalFilename: "book.epub")
        try await store.put(book)
        let reader = Data(repeating: 1, count: 16), generation = Data(repeating: 2, count: 16)
        let owner = Data(repeating: 3, count: 16)
        let manifest = try ContentManifest(content: content, kind: .epub, length: 123, formatVersion: 1, logicalIdentity: Data(count: 16))
        let inventory = try ReaderInventory(reader: reader, generation: generation, contents: [manifest], complete: true)
        let job = try await store.queueRemoval(manifest: manifest, inventory: inventory, installation: owner)
        let duplicate = try await store.queueRemoval(manifest: manifest, inventory: inventory, installation: owner)
        XCTAssertEqual(duplicate, job)
        let retained = try await store.content(content)
        XCTAssertEqual(retained, book)
        let selections = try await store.readerSelections(reader: reader)
        XCTAssertEqual(selections, [ReaderContentSelection(content: content, selected: false)])
        try await store.checkpointRemoval(job.id, phase: .removing)
        let reopened = try LibraryStore(url: url)
        let saved = try await reopened.removalJob(job.id)
        XCTAssertEqual(saved?.request, job.request)
        XCTAssertEqual(saved?.phase, .removing)
        try await reopened.setReaderSelection(reader: reader, content: content, selected: true)
        let selectedPending = try await reopened.pendingRemovalJobs()
        XCTAssertEqual(selectedPending.first?.request, job.request)
        do {
            _ = try await reopened.enqueueSelectedContent(content: content, reader: reader, storageGeneration: generation, installation: owner)
            XCTFail("An uncertain removal must finish before reinstall")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        do {
            _ = try await reopened.enqueue(content: content, reader: reader, storageGeneration: generation, installation: owner)
            XCTFail("Direct queueing must not bypass removal ownership")
        } catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        try await reopened.checkpointRemoval(job.id, phase: .paused)
        try await reopened.checkpointRemoval(job.id, phase: .removing)
        try await reopened.checkpointRemoval(job.id, phase: .completed)
        let pending = try await reopened.pendingRemovalJobs()
        XCTAssertTrue(pending.isEmpty)
        let emptyInventory = try ReaderInventory(reader: reader, generation: generation, contents: [], complete: true)
        let reconciliation = try await reopened.reconcileContent(reader: reader, generation: generation, inventory: emptyInventory)
        XCTAssertEqual(reconciliation, [.install(book)])
        let reinstall = try await reopened.enqueueSelectedContent(content: content, reader: reader, storageGeneration: generation, installation: owner)
        XCTAssertEqual(reinstall.content, content)
        do { try await reopened.checkpointRemoval(job.id, phase: .removing); XCTFail("Completed job must stay completed") }
        catch { XCTAssertEqual(error as? StoreError, .invalidTransition) }
    }
}

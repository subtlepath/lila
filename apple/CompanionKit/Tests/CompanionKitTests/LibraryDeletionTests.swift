import Foundation
import XCTest
@testable import CompanionKit

final class LibraryDeletionTests: XCTestCase, @unchecked Sendable {
    func testDeletionPreservesRecoveryAndRequiresExplicitRestore() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: url)
        let id = try ContentID(String(repeating: "a", count: 64))
        let content = LibraryContent(id: id, kind: .epub, length: 3, title: "Book", originalFilename: "book.epub")
        try await library.put(content)
        let first = Data(repeating: 1, count: 16), second = Data(repeating: 2, count: 16)
        let generation = Data(repeating: 3, count: 16), owner = Data(repeating: 4, count: 16)
        _ = try await library.setReaderSelection(reader: first, content: id, selected: true)
        _ = try await library.setReaderSelection(reader: second, content: id, selected: true)
        let queued = try await library.enqueueSelectedContent(content: id, reader: first, storageGeneration: generation, installation: owner)
        let committing = try await library.enqueueSelectedContent(content: id, reader: second, storageGeneration: generation, installation: owner)
        try await library.checkpoint(committing.id, offset: 3, phase: .committing)
        let deleted = try await library.deleteLibraryContent(id); XCTAssertTrue(deleted)
        let duplicate = try await library.deleteLibraryContent(id); XCTAssertFalse(duplicate)
        let reopened = try LibraryStore(url: url)
        let visible = try await reopened.libraryContentIDs(); XCTAssertEqual(visible, [])
        let retained = try await reopened.content(id); XCTAssertEqual(retained, content)
        let removedIDs = try await reopened.deletedLibraryContentIDs(); XCTAssertEqual(removedIDs, [id])
        let abort = try await reopened.hasTransferAbort(queued.id); XCTAssertTrue(abort)
        let commitAbort = try await reopened.hasTransferAbort(committing.id); XCTAssertFalse(commitAbort)
        let choices = try await reopened.readerSelections(reader: second)
        XCTAssertEqual(choices, [ReaderContentSelection(content: id, selected: false)])
        do { _ = try await reopened.setReaderSelection(reader: first, content: id, selected: true); XCTFail("Deleted content selected") }
        catch { XCTAssertEqual(error as? StoreError, .invalidTransition) }
        try await reopened.put(content)
        let stillDeleted = try await reopened.isLibraryContentDeleted(id); XCTAssertTrue(stillDeleted)
        var bytes = Data([1, 2]); bytes.append(id.digest); bytes.append(1)
        bytes.appendLittleEndian(3, count: 8); bytes.appendLittleEndian(1, count: 4); bytes.append(Data(count: 16))
        let remote = try ContentManifest(decoding: bytes)
        let unknownReader = Data(repeating: 5, count: 16)
        let inventory = try ReaderInventory(reader: unknownReader, generation: generation, contents: [remote], complete: true)
        let removal = try await reopened.reconcileContent(reader: unknownReader, generation: generation, inventory: inventory)
        XCTAssertEqual(removal, [.remove(remote)])
        try await reopened.restoreLibraryContent(id)
        let restored = try await reopened.libraryContentIDs(); XCTAssertEqual(restored, [id])
        let noRemovedIDs = try await reopened.deletedLibraryContentIDs(); XCTAssertTrue(noRemovedIDs.isEmpty)
        let stillDeselected = try await reopened.isReaderContentSelected(reader: second, content: id); XCTAssertFalse(stillDeselected)
        async let selection = try? library.setReaderSelection(reader: second, content: id, selected: true)
        async let deletion = reopened.deleteLibraryContent(id)
        _ = try await (selection, deletion)
        let finalDeleted = try await reopened.isLibraryContentDeleted(id); XCTAssertTrue(finalDeleted)
        let finalSelection = try await reopened.isReaderContentSelected(reader: second, content: id); XCTAssertFalse(finalSelection)

    }
}

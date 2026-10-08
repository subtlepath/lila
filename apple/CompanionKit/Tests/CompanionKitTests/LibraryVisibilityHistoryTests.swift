import Foundation
import XCTest
@testable import CompanionKit

final class LibraryVisibilityHistoryTests: XCTestCase, @unchecked Sendable {
    func testCloudVisibilityAcknowledgementsAreDurableAccountBoundAndAtomic() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite"), id = try content()
        let store = try LibraryStore(url: url)
        let remove = try LibraryVisibilityChange(origin: origin, content: id, removed: true, ancestors: [])
        let restore = try LibraryVisibilityChange(origin: origin, content: id, removed: false, ancestors: [remove.id])
        _ = try await store.importLibraryVisibilityChanges([remove])
        try await store.acknowledgeCloudVisibilityChanges([remove, remove], account: "first")
        let reopened = try LibraryStore(url: url)
        let known = try await reopened.pendingCloudVisibilityChanges([remove], account: "first"); XCTAssertTrue(known.isEmpty)
        let other = try await reopened.pendingCloudVisibilityChanges([remove], account: "second"); XCTAssertEqual(other, [remove])
        do { try await reopened.acknowledgeCloudVisibilityChanges([remove, restore], account: "second"); XCTFail() }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let rolledBack = try await reopened.pendingCloudVisibilityChanges([remove], account: "second"); XCTAssertEqual(rolledBack, [remove])
        let altered = try LibraryVisibilityChange(id: remove.id, origin: origin, content: id, removed: false, ancestors: [])
        do { _ = try await reopened.pendingCloudVisibilityChanges([altered], account: "first"); XCTFail() }
        catch { XCTAssertEqual(error as? LibraryVisibilityError, .equivocation(remove.id)) }
    }
    func testLegacyBackfillIsBoundedRestartSafeAndPreventsUnobservedRestore() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite"), first = try content(), second = try content("b")
        let store = try LibraryStore(url: url)
        for id in [first, second] {
            try await store.put(LibraryContent(id: id, kind: .epub, length: 10, title: "Book", originalFilename: "book.epub"))
            _ = try await store.deleteLibraryContent(id)
        }
        let one = try await store.journalLegacyLibraryRemovals(origin: origin, limit: 1); XCTAssertEqual(one, 1)
        let reopened = try LibraryStore(url: url)
        let two = try await reopened.journalLegacyLibraryRemovals(origin: origin, limit: 1); XCTAssertEqual(two, 1)
        let complete = try await reopened.journalLegacyLibraryRemovals(origin: origin, limit: 1); XCTAssertEqual(complete, 0)
        let changes = try await reopened.libraryVisibilityChanges(); XCTAssertEqual(changes.count, 2)
        XCTAssertTrue(changes.allSatisfy { $0.removed && $0.ancestors.isEmpty })
        let staleRestore = try LibraryVisibilityChange(origin: Data(repeating: 2, count: 16), content: first, removed: false, ancestors: [])
        _ = try await reopened.importLibraryVisibilityChanges([staleRestore])
        let retained = try await reopened.isLibraryContentDeleted(first); XCTAssertTrue(retained)
        _ = try await reopened.setLibraryVisibility(first, removed: false, origin: origin)
        let restored = try await reopened.isLibraryContentDeleted(first); XCTAssertFalse(restored)
    }
    func testVisibilityBeforeContentAppliesOnArrivalAndObservedRestoreKeepsItVisible() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite"), first = try content(), second = try content("b")
        let store = try LibraryStore(url: url)
        let removeFirst = try LibraryVisibilityChange(origin: origin, content: first, removed: true, ancestors: [])
        let removeSecond = try LibraryVisibilityChange(origin: origin, content: second, removed: true, ancestors: [])
        let restoreSecond = try LibraryVisibilityChange(origin: origin, content: second, removed: false, ancestors: [removeSecond.id])
        _ = try await store.importLibraryVisibilityChanges([removeFirst, restoreSecond, removeSecond])
        let reopened = try LibraryStore(url: url)
        for id in [first, second] {
            try await reopened.put(LibraryContent(id: id, kind: .epub, length: 10, title: "Book", originalFilename: "book.epub"))
        }
        let removed = try await reopened.isLibraryContentDeleted(first); XCTAssertTrue(removed)
        let restored = try await reopened.isLibraryContentDeleted(second); XCTAssertFalse(restored)
        let visible = try await reopened.libraryContentIDs(); XCTAssertEqual(visible, [second])
        let retained = try await reopened.content(first); XCTAssertNotNil(retained)
    }
    func testLocalActionsJournalObservedHeadsAndPreserveLegacyRemovalOnRestore() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite")), id = try content()
        try await store.put(LibraryContent(id: id, kind: .epub, length: 10, title: "Book", originalFilename: "book.epub"))
        _ = try await store.deleteLibraryContent(id)
        let restored = try await store.setLibraryVisibility(id, removed: false, origin: origin); XCTAssertTrue(restored)
        let initial = try await store.libraryVisibilityChanges(); XCTAssertEqual(initial.count, 2)
        let deletion = try XCTUnwrap(initial.first(where: \.removed))
        let restore = try XCTUnwrap(initial.first(where: { !$0.removed }))
        XCTAssertEqual(restore.ancestors, [deletion.id])
        let duplicate = try await store.setLibraryVisibility(id, removed: false, origin: origin); XCTAssertFalse(duplicate)
        _ = try await store.setLibraryVisibility(id, removed: true, origin: origin)
        let removed = try await store.isLibraryContentDeleted(id); XCTAssertTrue(removed)
        let changes = try await store.libraryVisibilityChanges(); XCTAssertEqual(changes.count, 3)
        let next = try XCTUnwrap(changes.first(where: { $0.id != deletion.id && $0.id != restore.id }))
        XCTAssertEqual(next.ancestors, [restore.id])
    }
    func testPersistedVisibilityConvergesAcrossRestartAndRejectsEquivocationAtomically() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite"), id = try content()
        let store = try LibraryStore(url: url)
        try await store.put(LibraryContent(id: id, kind: .epub, length: 10, title: "Book", originalFilename: "book.epub"))
        let remove = try LibraryVisibilityChange(origin: origin, content: id, removed: true, ancestors: [])
        let restore = try LibraryVisibilityChange(origin: origin, content: id, removed: false, ancestors: [remove.id])
        _ = try await store.importLibraryVisibilityChanges([restore])
        let deferred = try await store.isLibraryContentDeleted(id); XCTAssertFalse(deferred)
        let reopened = try LibraryStore(url: url)
        _ = try await reopened.importLibraryVisibilityChanges([remove])
        let restored = try await reopened.isLibraryContentDeleted(id); XCTAssertFalse(restored)
        let repeated = try await reopened.importLibraryVisibilityChanges([remove, restore]); XCTAssertEqual(repeated, 0)
        let next = try LibraryVisibilityChange(origin: origin, content: id, removed: true, ancestors: [restore.id])
        let conflicting = try LibraryVisibilityChange(id: remove.id, origin: origin, content: id, removed: false, ancestors: [])
        do { _ = try await reopened.importLibraryVisibilityChanges([next, conflicting]); XCTFail() }
        catch { XCTAssertEqual(error as? LibraryVisibilityError, .equivocation(remove.id)) }
        let events = try await reopened.libraryVisibilityChanges(); XCTAssertEqual(events.count, 2)
        _ = try await reopened.importLibraryVisibilityChanges([next])
        let removed = try await reopened.isLibraryContentDeleted(id); XCTAssertTrue(removed)
    }
    private let origin = Data(repeating: 1, count: 16)
    private func content(_ digit: String = "a") throws -> ContentID { try ContentID(String(repeating: digit, count: 64)) }
    func testConcurrentDeletionWinsAndObservedRestoreConvergesInAnyOrder() throws {
        let id = try content()
        let remove = try LibraryVisibilityChange(origin: origin, content: id, removed: true, ancestors: [])
        let restore = try LibraryVisibilityChange(origin: Data(repeating: 2, count: 16), content: id, removed: false, ancestors: [])
        let concurrent = try LibraryVisibilityHistory.merge([remove, restore, remove], content: id)
        XCTAssertTrue(concurrent.removed); XCTAssertEqual(concurrent.heads.count, 2)
        let resolved = try LibraryVisibilityChange(origin: origin, content: id, removed: false, ancestors: concurrent.heads)
        let forward = try LibraryVisibilityHistory.merge([remove, restore, resolved], content: id)
        let reverse = try LibraryVisibilityHistory.merge([resolved, restore, remove, resolved], content: id)
        XCTAssertEqual(forward, reverse); XCTAssertFalse(forward.removed); XCTAssertEqual(forward.heads, [resolved.id])
    }
    func testMissingAncestryDefersRestoreAndDoesNotEraseCurrentDeletion() throws {
        let id = try content()
        let remove = try LibraryVisibilityChange(origin: origin, content: id, removed: true, ancestors: [])
        let intermediate = try LibraryVisibilityChange(origin: origin, content: id, removed: true, ancestors: [remove.id])
        let restore = try LibraryVisibilityChange(origin: origin, content: id, removed: false, ancestors: [intermediate.id])
        let deferred = try LibraryVisibilityHistory.merge([restore, remove], content: id)
        XCTAssertTrue(deferred.removed); XCTAssertEqual(deferred.heads, [remove.id]); XCTAssertEqual(deferred.deferred, [restore.id])
        let complete = try LibraryVisibilityHistory.merge([restore, intermediate, remove], content: id)
        XCTAssertFalse(complete.removed); XCTAssertTrue(complete.deferred.isEmpty)
    }
    func testRejectsEquivocationCyclesAndCrossContentParents() throws {
        let id = try content(), firstID = UUID(), secondID = UUID()
        let first = try LibraryVisibilityChange(id: firstID, origin: origin, content: id, removed: true, ancestors: [])
        let altered = try LibraryVisibilityChange(id: firstID, origin: origin, content: id, removed: false, ancestors: [])
        XCTAssertThrowsError(try LibraryVisibilityHistory.merge([first, altered], content: id))
        let a = try LibraryVisibilityChange(id: firstID, origin: origin, content: id, removed: true, ancestors: [secondID])
        let b = try LibraryVisibilityChange(id: secondID, origin: origin, content: id, removed: true, ancestors: [firstID])
        XCTAssertThrowsError(try LibraryVisibilityHistory.merge([a, b], content: id))
        let foreign = try LibraryVisibilityChange(origin: origin, content: content("b"), removed: false, ancestors: [firstID])
        XCTAssertThrowsError(try LibraryVisibilityHistory.merge([first, foreign], content: id))
        XCTAssertThrowsError(try LibraryVisibilityChange(id: firstID, origin: origin, content: id, removed: true, ancestors: [firstID]))
    }
}

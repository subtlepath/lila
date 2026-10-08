import Foundation
import XCTest
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

final class BookmarkHistoryTests: XCTestCase, @unchecked Sendable {
    private let content = Data(repeating: 3, count: 32)
    private func body(_ name: String?, id: UInt8 = 9) throws -> BookmarkBody {
        try BookmarkBody(identity: Data(repeating: id, count: 16), value: name.map {
            try BookmarkValue(anchor: ReadingAnchor(spine: 1, visibleTextOffset: 42), name: $0, summary: "Excerpt")
        })
    }
    private func mutation(_ body: BookmarkBody, origin: UInt8, sequence: UInt64 = 1, ancestors: [EventIdentity] = []) throws -> JournalMutation {
        var bytes = Data([1, 3]) + Data(repeating: origin, count: 16)
        bytes.appendLittleEndian(1, count: 8); bytes.appendLittleEndian(sequence, count: 8)
        bytes.append(Data(repeating: 2, count: 16)); bytes.append(body.kind.rawValue); bytes.append(content)
        bytes.append(Data(SHA256.hash(data: body.encoded))); bytes.appendLittleEndian(1, count: 4)
        bytes.appendLittleEndian(0, count: 8); bytes.append(0); bytes.appendLittleEndian(0, count: 4)
        bytes.append(Data(count: 32)); bytes.append(UInt8(ancestors.count))
        for ancestor in ancestors { bytes.append(ancestor.storageKey) }
        return try JournalMutation(event: SyncEvent(decoding: bytes), body: body.encoded)
    }
    func testAdditionsMergeAndObservedDeletionRemainsTombstone() throws {
        let first = try mutation(body("First"), origin: 1)
        let other = try mutation(body("Other", id: 8), origin: 2)
        let deleted = try mutation(body(nil), origin: 1, sequence: 2)
        let result = try BookmarkHistory.reconcile([deleted, other, first, deleted], content: content)
        XCTAssertEqual(result.count, 2)
        XCTAssertFalse(result[0].isDeleted); XCTAssertTrue(result[1].isDeleted)
        XCTAssertEqual(result[1].resolutionAncestors, [deleted.event.identity])
        XCTAssertEqual(try BookmarkHistory.reconcile([first, other, deleted], content: content), result)
    }
    func testConcurrentEditAndDeleteRequireResolution() throws {
        let first = try mutation(body("First"), origin: 1)
        let edit = try mutation(body("Edited"), origin: 2, ancestors: [first.event.identity])
        let delete = try mutation(body(nil), origin: 1, sequence: 2)
        let conflict = try XCTUnwrap(BookmarkHistory.reconcile([first, edit, delete], content: content).first)
        XCTAssertTrue(conflict.requiresResolution); XCTAssertFalse(conflict.isDeleted)
        XCTAssertEqual(conflict.candidates.count, 2)
        let resolution = try mutation(body(nil), origin: 3, ancestors: conflict.resolutionAncestors)
        let resolved = try XCTUnwrap(BookmarkHistory.reconcile([resolution, delete, edit, first], content: content).first)
        XCTAssertTrue(resolved.isDeleted); XCTAssertFalse(resolved.requiresResolution)
    }
    func testConflictsSurviveSQLiteReopen() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let first = try mutation(body("One"), origin: 1), second = try mutation(body("Two"), origin: 2)
        let store = try LibraryStore(url: url); try await store.importEvents([first, second])
        let reopened = try LibraryStore(url: url)
        let states = try await reopened.bookmarks(content: content)
        XCTAssertTrue(try XCTUnwrap(states.first).requiresResolution)
        XCTAssertEqual(states, try BookmarkHistory.reconcile([second, first], content: content))
    }
    func testBodyUTF8BoundsAndTruncation() throws {
        for bookmark in [try body(nil), try body("Café 📖")] {
            XCTAssertEqual(try BookmarkBody(decoding: bookmark.encoded), bookmark)
            for count in 0 ..< bookmark.encoded.count { XCTAssertThrowsError(try BookmarkBody(decoding: bookmark.encoded.prefix(count))) }
            XCTAssertThrowsError(try BookmarkBody(decoding: bookmark.encoded + Data([0])))
        }
        XCTAssertThrowsError(try body(String(repeating: "é", count: 65)))
        XCTAssertThrowsError(try body("a\0b"))
        XCTAssertThrowsError(try body(nil, id: 0))
        var malformed = try body("x").encoded; malformed[26] = 255
        XCTAssertThrowsError(try BookmarkBody(decoding: malformed))
    }
}

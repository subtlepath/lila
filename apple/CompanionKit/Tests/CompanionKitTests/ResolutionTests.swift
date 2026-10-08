import Foundation
import XCTest
import CSQLite
@testable import CompanionKit

final class ResolutionTests: XCTestCase, @unchecked Sendable {
    private let content = Data(repeating: 3, count: 32), origin = Data(repeating: 50, count: 16)
    private func store(_ url: URL) throws -> LibraryStore { try LibraryStore(url: url, random: { Data(repeating: 4, count: $0) }) }
    private func populate(_ store: LibraryStore, count: Int = 9) async throws -> [EventIdentity] {
        var heads: [EventIdentity] = []; heads.reserveCapacity(count)
        for index in 1 ... count {
            let event = try await store.appendReadingPosition(origin: Data(repeating: UInt8(index), count: 16), content: content,
                                                               anchor: ReadingAnchor(spine: 0, visibleTextOffset: UInt32(index)))
            heads.append(event.event.identity)
        }
        return heads
    }
    private func sql(_ statement: String, url: URL) throws {
        var handle: OpaquePointer?
        guard sqlite3_open(url.path, &handle) == SQLITE_OK else { throw StoreError.invalidValue }
        defer { sqlite3_close(handle) }
        guard sqlite3_exec(handle, statement, nil, nil, nil) == SQLITE_OK else { throw StoreError.invalidValue }
    }
    func testNineHeadsResolveWithThreeBoundedCausalJoins() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite"), store = try store(root.appendingPathComponent("library.sqlite"))
        let heads = try await populate(store)
        let joins = try await store.resolveReadingPosition(origin: origin, content: content,
                                                           anchor: ReadingAnchor(spine: 0, visibleTextOffset: 5), expectedHeads: heads.reversed())
        XCTAssertEqual(joins.count, 3)
        XCTAssertEqual(joins.map { $0.event.ancestors.count }, [4, 4, 1])
        let reopened = try self.store(url)
        let resolved = try await reopened.readingPositions(content: content)
        XCTAssertFalse(resolved.requiresResolution)
        XCTAssertEqual(resolved.resolutionAncestors, [try XCTUnwrap(joins.last).event.identity])
        XCTAssertEqual(resolved.candidates.first?.anchor.visibleTextOffset, 5)
    }
    func testStaleOrDuplicateFrontierDoesNotCreateEvents() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try store(root.appendingPathComponent("library.sqlite"))
        let heads = try await populate(store, count: 2)
        for expected in [[heads[0]], heads + [heads[0]], []] {
            do { _ = try await store.resolveReadingPosition(origin: origin, content: content, anchor: ReadingAnchor(spine: 0, visibleTextOffset: 1), expectedHeads: expected); XCTFail() }
            catch { XCTAssertEqual(error as? HistoryError, .staleFrontier) }
        }
        let events = try await store.syncEvents(); XCTAssertEqual(events.count, 2)
    }
    func testFailedSecondJoinRollsBackFirstJoinAndCounter() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite"), store = try store(root.appendingPathComponent("library.sqlite"))
        let heads = try await populate(store)
        try sql("""
            CREATE TRIGGER reject_second_join BEFORE INSERT ON sync_events
            WHEN substr(NEW.identity,1,16)=x'32323232323232323232323232323232'
                AND substr(NEW.identity,25,8)=x'0200000000000000'
            BEGIN SELECT RAISE(ABORT,'injected write failure'); END;
            """, url: url)
        do { _ = try await store.resolveReadingPosition(origin: origin, content: content, anchor: ReadingAnchor(spine: 0, visibleTextOffset: 1), expectedHeads: heads); XCTFail() }
        catch { guard case StoreError.database = error else { return XCTFail("Unexpected error: \(error)") } }
        let after = try await store.syncEvents(); XCTAssertEqual(after.count, 9)
        let conflict = try await store.readingPositions(content: content); XCTAssertEqual(Set(conflict.resolutionAncestors), Set(heads))
        try sql("DROP TRIGGER reject_second_join", url: url)
        let event = try await store.appendReadingPosition(origin: origin, content: content, anchor: ReadingAnchor(spine: 0, visibleTextOffset: 1))
        XCTAssertEqual(event.event.identity.sequence, 1)
    }
    func testBookmarkAndPreferenceResolutionsUseTheirOwnFrontiers() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try store(root.appendingPathComponent("library.sqlite"))
        let bookmarkID = Data(repeating: 9, count: 16)
        for index in 1 ... 2 {
            let source = Data(repeating: UInt8(index), count: 16)
            let bookmark = try BookmarkBody(identity: bookmarkID, value: BookmarkValue(anchor: ReadingAnchor(spine: 0, visibleTextOffset: 1), name: "Name \(index)"))
            _ = try await store.appendBookmark(origin: source, content: content, bookmark: bookmark)
            _ = try await store.appendPreference(origin: source, preference: PreferenceBody(key: .margin, value: .integer(Int32(index * 5))))
        }
        let bookmarkStates = try await store.bookmarks(content: content)
        let bookmark = try XCTUnwrap(bookmarkStates.first)
        let deletion = try BookmarkBody(identity: bookmarkID, value: nil)
        _ = try await store.resolveBookmark(origin: origin, content: content, bookmark: deletion, expectedHeads: bookmark.resolutionAncestors)
        let deleted = try await store.bookmarks(content: content)
        XCTAssertTrue(try XCTUnwrap(deleted.first).isDeleted)
        let preferences = try await store.preferences(), preference = try XCTUnwrap(preferences.first)
        _ = try await store.resolvePreference(origin: origin, preference: PreferenceBody(key: .margin, value: .integer(10)), expectedHeads: preference.resolutionAncestors)
        let resolved = try await store.preferences()
        XCTAssertFalse(try XCTUnwrap(resolved.first).requiresResolution)
        XCTAssertEqual(resolved.first?.candidates.first?.body.value, .integer(10))
    }

    func testTintaPreferenceChoiceJoinsEveryHeadAndRejectsStaleDisplayedHistory() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let store = try store(url)
        for index in 1 ... 9 {
            _ = try await store.appendPreference(origin: Data(repeating: UInt8(index), count: 16),
                preference: PreferenceBody(key: .tintaNewPerDay, value: .integer(Int32(index * 5))))
        }
        let displayed = try await store.preferences()
        let state = try XCTUnwrap(displayed.first)
        XCTAssertTrue(state.requiresResolution)
        XCTAssertEqual(state.candidates.count, 9)
        let choice = try XCTUnwrap(state.candidates.first { $0.body.value == .integer(20) })
        let resolutions = try await store.resolvePreference(origin: origin, preference: choice.body,
            expectedHeads: state.resolutionAncestors)
        XCTAssertEqual(resolutions.count, 3)
        let resolved = try await store.preferences()
        XCTAssertFalse(try XCTUnwrap(resolved.first).requiresResolution)
        XCTAssertEqual(resolved.first?.candidates.first?.body.value, .integer(20))
        let reopened = try self.store(url)
        let persisted = try await reopened.preferences()
        XCTAssertEqual(persisted, resolved)
        let before = try await reopened.syncEvents()
        do {
            _ = try await reopened.resolvePreference(origin: origin, preference: choice.body,
                expectedHeads: state.resolutionAncestors)
            XCTFail("A stale displayed conflict must not create a resolution")
        } catch {
            XCTAssertEqual(error as? HistoryError, .staleFrontier)
        }
        let after = try await reopened.syncEvents()
        XCTAssertEqual(after, before)
    }


}

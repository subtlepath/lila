import Foundation
import XCTest
@testable import CompanionKit

final class LocalEventTests: XCTestCase, @unchecked Sendable {
    private let origin = Data(repeating: 7, count: 16), content = Data(repeating: 3, count: 32)
    private func store(_ url: URL, seed: UInt8 = 4) throws -> LibraryStore {
        try LibraryStore(url: url, random: { Data(repeating: seed, count: $0) })
    }
    func testRestartContinuesCounterAndResolvesReadingConflict() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let firstStore = try store(url)
        let first = try await firstStore.appendReadingPosition(origin: origin, content: content, anchor: ReadingAnchor(spine: 0, visibleTextOffset: 10))
        let other = try await firstStore.appendReadingPosition(origin: Data(repeating: 8, count: 16), content: content, anchor: ReadingAnchor(spine: 0, visibleTextOffset: 20))
        let reopened = try store(url, seed: 9)
        let conflict = try await reopened.readingPositions(content: content)
        XCTAssertTrue(conflict.requiresResolution)
        let resolved = try await reopened.appendReadingPosition(origin: origin, content: content, anchor: ReadingAnchor(spine: 0, visibleTextOffset: 20), ancestors: conflict.resolutionAncestors)
        XCTAssertEqual(resolved.event.identity.epoch, first.event.identity.epoch)
        XCTAssertEqual(resolved.event.storageGeneration, first.event.storageGeneration)
        XCTAssertEqual(resolved.event.identity.sequence, 2)
        let positions = try await reopened.readingPositions(content: content)
        XCTAssertFalse(positions.requiresResolution); XCTAssertEqual(positions.candidates.count, 1)
        XCTAssertEqual(Set(resolved.event.ancestors), [first.event.identity, other.event.identity])
    }
    func testRejectedDependencyDoesNotConsumeSequence() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try store(root.appendingPathComponent("library.sqlite"))
        let preference = try PreferenceBody(key: .margin, value: .integer(10))
        let first = try await store.appendPreference(origin: origin, preference: preference)
        let missing = try EventIdentity(origin: Data(repeating: 9, count: 16), epoch: 1, sequence: 1)
        do { _ = try await store.appendPreference(origin: origin, preference: preference, ancestors: [missing]); XCTFail() }
        catch { XCTAssertEqual(error as? HistoryError, .missingAncestor(missing)) }
        let second = try await store.appendPreference(origin: origin, preference: preference)
        XCTAssertEqual(second.event.identity.sequence, 2)
        let events = try await store.syncEvents(); XCTAssertEqual(events.count, 2)
    }
    func testConcurrentHandlesAllocateUniqueContiguousSequences() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let first = try store(url), second = try store(url)
        let preference = try PreferenceBody(key: .margin, value: .integer(5))
        let identities = try await withThrowingTaskGroup(of: EventIdentity.self) { group in
            for index in 0 ..< 20 {
                let store = index.isMultiple(of: 2) ? first : second
                group.addTask { try await store.appendPreference(origin: self.origin, preference: preference).event.identity }
            }
            var identities: [EventIdentity] = []; identities.reserveCapacity(20)
            for try await identity in group { identities.append(identity) }
            return identities
        }
        XCTAssertEqual(Set(identities).count, 20)
        XCTAssertEqual(identities.map(\.sequence).sorted(), Array(1 ... 20).map(UInt64.init))
    }
    func testResetDatabaseReceivesNewEpochAndGeneration() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let original = try store(root.appendingPathComponent("first.sqlite"), seed: 4)
        let reset = try store(root.appendingPathComponent("reset.sqlite"), seed: 5)
        let bookmark = try BookmarkBody(identity: Data(repeating: 1, count: 16), value: nil)
        let first = try await original.appendBookmark(origin: origin, content: content, bookmark: bookmark)
        let second = try await reset.appendBookmark(origin: origin, content: content, bookmark: bookmark)
        XCTAssertNotEqual(first.event.identity, second.event.identity)
        XCTAssertNotEqual(first.event.storageGeneration, second.event.storageGeneration)
    }
    func testUnrelatedPartialRemoteHistoryDoesNotBlockLocalOfflineEvent() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let remote = try store(root.appendingPathComponent("remote.sqlite")), local = try store(root.appendingPathComponent("local.sqlite"))
        let preference = try PreferenceBody(key: .margin, value: .integer(5)), remoteOrigin = Data(repeating: 8, count: 16)
        _ = try await remote.appendPreference(origin: remoteOrigin, preference: preference)
        let tail = try await remote.appendPreference(origin: remoteOrigin, preference: preference)
        try await local.importEvents([tail])
        let created = try await local.appendPreference(origin: origin, preference: preference)
        XCTAssertEqual(created.event.identity.sequence, 1)
        let stored = try await local.storedEvent(created.event.identity); XCTAssertEqual(stored, created)
    }

}

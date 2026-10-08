import Foundation
import XCTest
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

final class JournalStoreTests: XCTestCase, @unchecked Sendable {
    private func mutation(sequence: UInt64, body: Data = Data([42])) throws -> JournalMutation {
        var bytes = Data([1, 3]) + Data(repeating: 1, count: 16)
        bytes.appendLittleEndian(UInt64.max, count: 8); bytes.appendLittleEndian(sequence, count: 8)
        bytes.append(Data(repeating: 2, count: 16)); bytes.append(SyncEventKind.readingPosition.rawValue)
        bytes.append(Data(repeating: 3, count: 32)); bytes.append(Data(SHA256.hash(data: body)))
        bytes.appendLittleEndian(1, count: 4); bytes.appendLittleEndian(0, count: 8); bytes.append(0)
        bytes.appendLittleEndian(0, count: 4); bytes.append(Data(count: 32)); bytes.append(0)
        return try JournalMutation(event: SyncEvent(decoding: bytes), body: body)
    }
    func testRestartDedupAndOutOfOrderDelivery() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let first = try mutation(sequence: 1), second = try mutation(sequence: 2)
        let store = try LibraryStore(url: url)
        let count = try await store.importEvents([second, first, second]); XCTAssertEqual(count, 2)
        let reopened = try LibraryStore(url: url)
        let duplicate = try await reopened.importEvents([first, second]); XCTAssertEqual(duplicate, 0)
        let events = try await reopened.syncEvents()
        XCTAssertEqual(try SyncHistory.merged(events), [first.event, second.event])
        let stored = try await reopened.storedEvent(first.event.identity); XCTAssertEqual(stored, first)
        let filtered = try await reopened.syncEvents(resource: Data(repeating: 9, count: 32)); XCTAssertTrue(filtered.isEmpty)
    }
    func testConflictRollsBackWholeBatch() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let first = try mutation(sequence: 1), second = try mutation(sequence: 2)
        try await store.importEvents([first])
        do { try await store.importEvents([second, mutation(sequence: 1, body: Data([99]))]); XCTFail() }
        catch { XCTAssertEqual(error as? HistoryError, .equivocation(first.event.identity)) }
        let missing = try await store.storedEvent(second.event.identity); XCTAssertNil(missing)
        let original = try await store.storedEvent(first.event.identity); XCTAssertEqual(original, first)
    }
    func testBoundedMutationScanRestartsForLateArrivalsAndSurvivesRestart() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let store = try LibraryStore(url: url)
        let entries = try [1, 2, 256, 257, 65536].map { try mutation(sequence: UInt64($0)) }
        _ = try await store.importEvents(Array(entries.reversed()))
        let reopened = try LibraryStore(url: url)
        var scanned: [JournalMutation] = []
        var cursor: EventIdentity?
        while true {
            let page = try await reopened.syncMutations(after: cursor, limit: 2)
            XCTAssertLessThanOrEqual(page.count, 2)
            if page.isEmpty { break }
            scanned.append(contentsOf: page); cursor = page.last?.event.identity
        }
        XCTAssertEqual(scanned, entries.sorted { $0.event.identity.storageKey.lexicographicallyPrecedes($1.event.identity.storageKey) })
        XCTAssertEqual(Set(scanned.map { $0.event.identity }), Set(entries.map { $0.event.identity }))
        let filtered = try await reopened.syncMutations(limit: 2, resource: Data(repeating: 9, count: 32))
        XCTAssertTrue(filtered.isEmpty)
        let late = try mutation(sequence: 0x01000000)
        _ = try await reopened.importEvents([late])
        let restarted = try await reopened.syncMutations(limit: 250)
        XCTAssertEqual(restarted.count, 6); XCTAssertTrue(restarted.contains(late))
        for limit in [0, 251] {
            do { _ = try await reopened.syncMutations(limit: limit); XCTFail("Unbounded page accepted") }
            catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        }
    }
    func testBodyIntegrityAndBounds() throws {
        let original = try mutation(sequence: 1)
        XCTAssertThrowsError(try JournalMutation(event: original.event, body: Data([0])))
        XCTAssertThrowsError(try mutation(sequence: 1, body: Data(count: JournalMutation.maximumBodySize + 1)))
        let empty = try mutation(sequence: 1, body: Data()); XCTAssertTrue(empty.body.isEmpty)
    }
    func testCloudRecordNameAndBodyMustMatchImmutableEvent() throws {
        let entry = try mutation(sequence: 256)
        let record = CloudJournalRecord(entry)
        XCTAssertEqual(record.name.count, 64)
        XCTAssertEqual(try CloudJournalRecord(name: record.name, envelope: entry.event.bytes, body: entry.body), record)
        XCTAssertThrowsError(try CloudJournalRecord(name: String(repeating: "0", count: 64),
                                                   envelope: entry.event.bytes, body: entry.body))
        XCTAssertThrowsError(try CloudJournalRecord(name: record.name.uppercased(),
                                                   envelope: entry.event.bytes, body: Data([99])))
        XCTAssertThrowsError(try CloudJournalRecord(name: record.name, envelope: Data(), body: entry.body))
    }
    func testCloudAcknowledgementsPersistPerAccountAndRollbackFailedBatch() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let store = try LibraryStore(url: url)
        let first = try mutation(sequence: 1), second = try mutation(sequence: 2), missing = try mutation(sequence: 3)
        _ = try await store.importEvents([first, second])
        try await store.acknowledgeCloudMutations([first, first], account: "first-account")
        let reopened = try LibraryStore(url: url)
        let pending = try await reopened.pendingCloudMutations([first, second], account: "first-account")
        XCTAssertEqual(pending, [second])
        let otherAccount = try await reopened.pendingCloudMutations([first, second], account: "second-account")
        XCTAssertEqual(otherAccount, [first, second])
        do {
            try await reopened.acknowledgeCloudMutations([second, missing], account: "first-account")
            XCTFail("Missing local event acknowledged")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let afterFailure = try await reopened.pendingCloudMutations([first, second], account: "first-account")
        XCTAssertEqual(afterFailure, [second])
        let altered = try mutation(sequence: 1, body: Data([99]))
        do { _ = try await reopened.pendingCloudMutations([altered], account: "first-account"); XCTFail() }
        catch { XCTAssertEqual(error as? HistoryError, .equivocation(first.event.identity)) }
        try await reopened.acknowledgeCloudMutations([second], account: "first-account")
        let complete = try await reopened.pendingCloudMutations([first, second], account: "first-account")
        XCTAssertTrue(complete.isEmpty)
        let events = try await reopened.syncEvents(); XCTAssertEqual(events.count, 2)
    }
    func testConcurrentHandlesDeduplicateAtomically() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let first = try LibraryStore(url: url), second = try LibraryStore(url: url)
        let entry = try mutation(sequence: 1)
        let inserted = try await withThrowingTaskGroup(of: Int.self) { group in
            for index in 0 ..< 20 {
                let store = index.isMultiple(of: 2) ? first : second
                group.addTask { try await store.importEvents([entry]) }
            }
            var total = 0
            for try await count in group { total += count }
            return total
        }
        XCTAssertEqual(inserted, 1)
        let events = try await first.syncEvents(); XCTAssertEqual(events, [entry.event])
    }

}

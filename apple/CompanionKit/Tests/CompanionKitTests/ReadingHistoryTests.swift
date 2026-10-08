import Foundation
import XCTest
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

final class ReadingHistoryTests: XCTestCase, @unchecked Sendable {
    private let content = Data(repeating: 3, count: 32)
    private func position(origin: UInt8, sequence: UInt64 = 1, offset: UInt32, ancestors: [EventIdentity] = [], timestamp: UInt64 = 0) throws -> JournalMutation {
        let body = ReadingAnchor(spine: 2, visibleTextOffset: offset).encoded
        var bytes = Data([1, 3]) + Data(repeating: origin, count: 16)
        bytes.appendLittleEndian(1, count: 8); bytes.appendLittleEndian(sequence, count: 8)
        bytes.append(Data(repeating: 2, count: 16)); bytes.append(1); bytes.append(content)
        bytes.append(Data(SHA256.hash(data: body))); bytes.appendLittleEndian(1, count: 4)
        bytes.appendLittleEndian(timestamp, count: 8); bytes.append(2)
        bytes.appendLittleEndian(0, count: 4); bytes.append(Data(count: 32)); bytes.append(UInt8(ancestors.count))
        for ancestor in ancestors { bytes.append(ancestor.storageKey) }
        return try JournalMutation(event: SyncEvent(decoding: bytes), body: body)
    }
    func testConcurrentPositionsRetainBothDespiteClockDifference() throws {
        let first = try position(origin: 1, offset: 100, timestamp: UInt64.max)
        let second = try position(origin: 2, offset: 200, timestamp: 1)
        let result = try ReadingHistory.reconcile([second, first, first], content: content)
        XCTAssertTrue(result.requiresResolution)
        XCTAssertEqual(result.candidates.map(\.anchor.visibleTextOffset), [100, 200])
        XCTAssertEqual(try ReadingHistory.reconcile([first, second], content: content), result)
    }
    func testOneSidedChangeAndExplicitResolutionSupersedeAncestors() throws {
        let first = try position(origin: 1, offset: 100)
        let next = try position(origin: 1, sequence: 2, offset: 200)
        let unilateral = try ReadingHistory.reconcile([next, first], content: content)
        XCTAssertEqual(unilateral.candidates.map(\.identity), [next.event.identity])
        let other = try position(origin: 2, offset: 300)
        let conflict = try ReadingHistory.reconcile([first, next, other], content: content)
        let resolution = try position(origin: 3, offset: 200, ancestors: conflict.resolutionAncestors)
        let resolved = try ReadingHistory.reconcile([other, resolution, next, first], content: content)
        XCTAssertFalse(resolved.requiresResolution)
        XCTAssertEqual(resolved.candidates.map(\.identity), [resolution.event.identity])
    }
    func testEqualConcurrentAnchorsKeepBothCausalHeads() throws {
        let first = try position(origin: 1, offset: 100), second = try position(origin: 2, offset: 100)
        let result = try ReadingHistory.reconcile([first, second], content: content)
        XCTAssertFalse(result.requiresResolution); XCTAssertEqual(result.resolutionAncestors.count, 2)
    }
    func testAnchorBoundsAndIncompleteHistory() throws {
        let anchor = ReadingAnchor(spine: UInt16.max, visibleTextOffset: UInt32.max)
        XCTAssertEqual(try ReadingAnchor(decoding: anchor.encoded), anchor)
        XCTAssertThrowsError(try ReadingAnchor(decoding: anchor.encoded.dropLast()))
        XCTAssertThrowsError(try ReadingAnchor(decoding: anchor.encoded + Data([0])))
        let next = try position(origin: 1, sequence: 2, offset: 100)
        XCTAssertThrowsError(try ReadingHistory.reconcile([next], content: content))
    }
    func testConflictSurvivesSQLiteReopen() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let first = try position(origin: 1, offset: 100), second = try position(origin: 2, offset: 200)
        let store = try LibraryStore(url: url)
        try await store.importEvents([first, second])
        let reopened = try LibraryStore(url: url)
        let result = try await reopened.readingPositions(content: content)
        XCTAssertTrue(result.requiresResolution)
        XCTAssertEqual(result.candidates.count, 2)
        XCTAssertEqual(result, try ReadingHistory.reconcile([first, second], content: content))
    }

}

import Foundation
import XCTest
@testable import CompanionKit

final class LegacyMarkLogTests: XCTestCase {
    private func course(stories: [UInt32] = [1, 2, 3], keys: [UInt32] = [7, 7, 9]) -> CoursePackMetadata {
        CoursePackMetadata(major: 1, minor: 0, contentVersion: 1, locale: "es",
                           itemIdentities: [], recognitionItems: [], storyIdentities: stories,
                           lessonIdentities: [], lessonCount: 0, legacyStoryIdentities: keys)
    }
    func testReadingSnapshotRejectsPartialCollisionsAndUnknownIdentities() throws {
        let metadata = course()
        XCTAssertEqual(try LegacyMarkLog(bytes: LegacyMarkLog.encodeCompletedReadings(
            identities: [1, 2, 3], course: metadata)).keys, [7, 9])
        XCTAssertEqual(try LegacyMarkLog(bytes: LegacyMarkLog.encodeCompletedReadings(
            identities: [3], course: metadata)).keys, [9])
        XCTAssertEqual(try LegacyMarkLog(bytes: LegacyMarkLog.encodeCompletedReadings(
            identities: [], course: metadata)).keys, [])
        XCTAssertThrowsError(try LegacyMarkLog.encodeCompletedReadings(identities: [1], course: metadata)) {
            XCTAssertEqual($0 as? TintaReadingSnapshotError, .partialCollision(7))
        }
        XCTAssertThrowsError(try LegacyMarkLog.encodeCompletedReadings(identities: [4], course: metadata)) {
            XCTAssertEqual($0 as? TintaReadingSnapshotError, .unknownIdentity(4))
        }
        for malformed in [course(stories: [1, 1, 3]), course(keys: [7])] {
            XCTAssertThrowsError(try LegacyMarkLog.encodeCompletedReadings(identities: [], course: malformed)) {
                XCTAssertEqual($0 as? TintaReadingSnapshotError, .invalidMapping)
            }
        }
    }
    func testSnapshotEncoderMatchesIndependentFixtureAndCapacity() throws {
        XCTAssertEqual(try LegacyMarkLog.encodeSnapshot(keys: [7]),
                       Data([0x54, 0x4d, 0x4b, 0x31, 7, 0, 0, 0, 1, 0, 0x5a, 0xa0]))
        XCTAssertEqual(try LegacyMarkLog.encodeSnapshot(keys: []), Data("TMK1".utf8))
        let keys = Set(UInt32(1)...UInt32(96))
        let encoded = try LegacyMarkLog.encodeSnapshot(keys: keys)
        let decoded = try LegacyMarkLog(bytes: encoded)
        XCTAssertEqual(decoded.keys, keys.sorted())
        XCTAssertEqual(decoded.recordCount, 96)
        XCTAssertThrowsError(try LegacyMarkLog.encodeSnapshot(keys: Set(UInt32(1)...UInt32(97))))
    }
    private func record(_ key: UInt32, operation: UInt8 = 1) -> Data {
        var bytes = Data(); bytes.appendLittleEndian(UInt64(key), count: 4)
        bytes.append(operation); bytes.append(0)
        bytes.appendLittleEndian(UInt64(legacyCRC32(bytes) & 0xffff), count: 2)
        return bytes
    }
    func testSetReplayPreservesReaderOrder() throws {
        let bytes = Data("TMK1".utf8) + record(1) + record(2) + record(1) + record(1, operation: 2) + record(1)
        let decoded = try LegacyMarkLog(bytes: bytes)
        XCTAssertEqual(decoded.keys, [2, 1]); XCTAssertEqual(decoded.recordCount, 5)
        XCTAssertEqual(decoded.originalBytes, bytes)
        XCTAssertEqual(try LegacyMarkLog(bytes: Data("TMK1".utf8)).keys, [])
    }
    func testTornUnknownAndOversizedSetsRequireRecovery() throws {
        var bytes = Data("TMK1".utf8) + record(1); bytes[10] ^= 1
        XCTAssertThrowsError(try LegacyMarkLog(bytes: bytes)) {
            XCTAssertEqual($0 as? LegacyMarkLogError, .invalidRecord(0))
        }
        XCTAssertThrowsError(try LegacyMarkLog(bytes: Data("TMK1".utf8) + Data([1]))) {
            XCTAssertEqual($0 as? LegacyMarkLogError, .truncated)
        }
        XCTAssertThrowsError(try LegacyMarkLog(bytes: Data("TMK1".utf8) + record(1, operation: 3)))
        bytes = Data("TMK1".utf8); bytes.reserveCapacity(4 + 97 * 8)
        for key in UInt32(1)...UInt32(97) { bytes.append(record(key)) }
        XCTAssertThrowsError(try LegacyMarkLog(bytes: bytes)) {
            XCTAssertEqual($0 as? LegacyMarkLogError, .capacityExceeded(96))
        }
    }
}

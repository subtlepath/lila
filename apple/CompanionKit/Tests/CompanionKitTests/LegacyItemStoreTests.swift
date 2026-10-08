import Foundation
import XCTest
@testable import CompanionKit

final class LegacyItemStoreTests: XCTestCase {
    func testSnapshotEncoderMatchesProductionReaderFixture() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let fixture = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/TintaItemSnapshot-v1.fixture"))
        let items: [UInt32: ScheduledItem] = [1000: try ScheduledItem(uid: 1000), 1001: try ScheduledItem(uid: 1001)]
        XCTAssertEqual(try LegacyItemStore.encodeSnapshot(items: items, studyDay: 42, newItems: 7, reviews: 11), fixture)
        let files = try TintaDerivedCourseFiles(course: Data(repeating: 7, count: 16), studyDay: 42,
            itemState: fixture, localReviews: Data(),
            completedLessons: TintaCompletionSet(kind: .lessons, identities: []).encoded,
            completedReadings: TintaCompletionSet(kind: .readings, identities: []).encoded,
            dayLog: Data("TDL1".utf8))
        let manifest = try files.installationManifest(storageGeneration: Data(repeating: 1, count: 16),
            snapshotIdentity: Data(repeating: 2, count: 16), packHash: Data(repeating: 3, count: 32),
            frontierHash: Data(repeating: 4, count: 32), revision: 1)
        XCTAssertEqual(manifest, try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/TintaDerivedManifest-v1.fixture")))
    }
    func testSnapshotEncoderPreservesStatesAndCleanHeaderCounters() throws {
        let fresh = try ScheduledItem(uid: 9)
        let reviewed = try ScheduledItem(uid: 3).reviewed(grade: 4, day: 42, configuration: SchedulerConfiguration())
        var bytes = try LegacyItemStore.encodeSnapshot(items: [9: fresh, 3: reviewed], studyDay: 42,
                                                      newItems: 7, reviews: 11)
        let decoded = try LegacyItemStore(bytes: bytes)
        XCTAssertEqual(decoded.items, [9: fresh, 3: reviewed])
        XCTAssertEqual(decoded.sequence, 2); XCTAssertEqual(decoded.journalCount, 0)
        XCTAssertEqual(Data(bytes[1024..<1040]), reviewed.bytes)
        XCTAssertEqual(Data(bytes[1040..<1056]), fresh.bytes)
        for offset in [0, 512] {
            var fields = ByteReader(Data(bytes[(offset + 20)..<(offset + 28)]))
            XCTAssertEqual(try fields.number(2), 42)
            XCTAssertEqual(try fields.number(2), 7)
            XCTAssertEqual(try fields.number(2), 11)
            XCTAssertEqual(try fields.number(2), 0)
        }
        bytes[588] ^= 1
        XCTAssertEqual(try LegacyItemStore(bytes: bytes).sequence, 1)
        XCTAssertThrowsError(try LegacyItemStore.encodeSnapshot(items: [4: reviewed], studyDay: 42, newItems: 0, reviews: 0))
        let empty = try LegacyItemStore.encodeSnapshot(items: [:], studyDay: 0, newItems: 0, reviews: 0)
        XCTAssertEqual(empty.count, 1024)
        XCTAssertTrue(try LegacyItemStore(bytes: empty).items.isEmpty)
    }
    private func header(sequence: UInt32, count: UInt32, flags: UInt16 = 0) -> Data {
        var bytes = Data("TIS1".utf8)
        bytes.appendLittleEndian(1, count: 2); bytes.appendLittleEndian(80, count: 2)
        bytes.appendLittleEndian(UInt64(sequence), count: 4); bytes.appendLittleEndian(UInt64(count), count: 4)
        bytes.appendLittleEndian(2, count: 4); bytes.append(Data(count: 6))
        bytes.appendLittleEndian(UInt64(flags), count: 2); bytes.appendLittleEndian(0, count: 4)
        bytes.append(Data(count: 44)); bytes.appendLittleEndian(UInt64(legacyCRC32(bytes)), count: 4)
        return bytes
    }
    private func file(_ records: [Data], pending: Bool = false) -> Data {
        var bytes = Data(count: 1024)
        bytes.replaceSubrange(0..<80, with: header(sequence: 1, count: UInt32(records.count)))
        bytes.replaceSubrange(512..<592, with: header(sequence: 2, count: UInt32(records.count), flags: pending ? 1 : 0))
        for record in records { bytes.append(record) }
        return bytes
    }
    func testCleanSnapshotAndDamagedAlternateHeader() throws {
        XCTAssertEqual(legacyCRC32(Data("123456789".utf8)), 0xcbf43926)
        let item = try ScheduledItem(uid: 1).reviewed(grade: 3, day: 10, configuration: SchedulerConfiguration())
        var bytes = file([item.bytes])
        XCTAssertEqual(try LegacyItemStore(bytes: bytes).items, [1: item])
        XCTAssertEqual(try LegacyItemStore(bytes: bytes).sequence, 2)
        bytes[588] ^= 1
        XCTAssertEqual(try LegacyItemStore(bytes: bytes).sequence, 1)
    }
    func testPendingRecoveryAmbiguityAndRetiredSlots() throws {
        let item = try ScheduledItem(uid: 1)
        var bytes = file([item.bytes], pending: true)
        var pending = Data(bytes[512..<592])
        pending.replaceSubrange(32..<48, with: item.bytes)
        var crc = Data(); crc.appendLittleEndian(UInt64(legacyCRC32(Data(pending[0..<76]))), count: 4)
        pending.replaceSubrange(76..<80, with: crc)
        bytes.replaceSubrange(512..<592, with: pending)
        XCTAssertThrowsError(try LegacyItemStore(bytes: bytes)) {
            XCTAssertEqual($0 as? LegacyItemStoreError, .recoveryRequired)
        }
        bytes = file([item.bytes])
        bytes.replaceSubrange(0..<80, with: header(sequence: 2, count: 0))
        XCTAssertThrowsError(try LegacyItemStore(bytes: bytes)) {
            XCTAssertEqual($0 as? LegacyItemStoreError, .ambiguousHeader)
        }
        var retired = Data(repeating: 0xff, count: 4); retired.append(Data(count: 12))
        let decoded = try LegacyItemStore(bytes: file([item.bytes, retired]))
        XCTAssertEqual(decoded.retiredRecords, 1); XCTAssertEqual(decoded.items, [1: item])
    }
    func testDamagedDuplicateAndTruncatedRecordsRejected() throws {
        let item = try ScheduledItem(uid: 1)
        XCTAssertThrowsError(try LegacyItemStore(bytes: file([item.bytes, item.bytes]))) {
            XCTAssertEqual($0 as? LegacyItemStoreError, .duplicateUID(1))
        }
        var bytes = file([item.bytes]); bytes.removeLast()
        XCTAssertThrowsError(try LegacyItemStore(bytes: bytes)) {
            XCTAssertEqual($0 as? LegacyItemStoreError, .truncatedRecords)
        }
        bytes = file([item.bytes]); bytes[1035] = 0xff
        XCTAssertThrowsError(try LegacyItemStore(bytes: bytes))
    }
}

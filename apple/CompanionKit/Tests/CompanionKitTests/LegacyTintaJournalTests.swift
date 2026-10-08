import Foundation
import XCTest
@testable import CompanionKit

final class LegacyTintaJournalTests: XCTestCase {
    func testSharedNativeStreamingFixture() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let bytes = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/LegacyTintaJournal-v1.fixture"))
        let journal = try LegacyTintaJournal(courseIdentity: Data(repeating: 7, count: 16), bytes: bytes)
        XCTAssertEqual(journal.entries.count, 3)
        XCTAssertEqual(journal.entries[0].operation, .review(grade: 3, format: 2, responseQuarterSeconds: 4))
        XCTAssertEqual(journal.entries[0].timestamp, 7200)
        XCTAssertEqual(journal.entries[1].operation, .undo(reviewRecord: 0))
        XCTAssertEqual(journal.entries[2].operation, .setFlags(2))
        XCTAssertEqual(journal.zeroTailBytes, 12)
        XCTAssertEqual(journal.effectiveReviewCount, 0)
    }
    private let course = Data(repeating: 7, count: 16)
    private func record(uid: UInt32 = 1, time: UInt32 = 100, day: UInt16 = 10, op: UInt8 = 3, arg: UInt8 = 4) -> Data {
        var bytes = Data(); bytes.reserveCapacity(12)
        bytes.appendLittleEndian(UInt64(uid), count: 4); bytes.appendLittleEndian(UInt64(time), count: 4)
        bytes.appendLittleEndian(UInt64(day), count: 2); bytes.append(op); bytes.append(arg)
        return bytes
    }
    func testReviewUndoAndFlagSemantics() throws {
        let bytes = record() + record(op: 8, arg: 0) + record(uid: 2, op: 4, arg: 255) + record(uid: 2, op: 16, arg: 5)
        let journal = try LegacyTintaJournal(courseIdentity: course, bytes: bytes)
        XCTAssertEqual(journal.entries.count, 4); XCTAssertEqual(journal.effectiveReviewCount, 1)
        XCTAssertEqual(journal.entries[0].operation, .review(grade: 3, format: 0, responseQuarterSeconds: 4))
        XCTAssertEqual(journal.entries[1].operation, .undo(reviewRecord: 0))
        XCTAssertEqual(journal.entries[3].operation, .setFlags(5))
        XCTAssertEqual(journal.originalBytes, bytes); XCTAssertEqual(journal.originalHash.count, 32)
    }
    func testUndoRequiresLatestReviewAndIsClearedByControls() throws {
        for bytes in [record(op: 8, arg: 0), record() + record(uid: 2, op: 8, arg: 0),
                      record() + record(op: 8, arg: 0) + record(op: 8, arg: 0),
                      record() + record(op: 16, arg: 1) + record(op: 8, arg: 0)] {
            XCTAssertThrowsError(try LegacyTintaJournal(courseIdentity: course, bytes: bytes))
        }
    }
    func testRecoveredZeroTailIsPreservedAndNonzeroTornTailRejects() throws {
        let bytes = record() + Data(count: 27)
        let journal = try LegacyTintaJournal(courseIdentity: course, bytes: bytes)
        XCTAssertEqual(journal.zeroTailBytes, 27); XCTAssertEqual(journal.entries.count, 1)
        XCTAssertEqual(journal.originalBytes, bytes)
        XCTAssertThrowsError(try LegacyTintaJournal(courseIdentity: course, bytes: record() + Data([1])))
        XCTAssertThrowsError(try LegacyTintaJournal(courseIdentity: course, bytes: record() + Data(count: 12) + record()))
    }
    func testMatchingAndPrefixHistoriesAlwaysRequireConfirmation() throws {
        let first = try LegacyTintaJournal(courseIdentity: course, bytes: record())
        let identical = try LegacyTintaJournal(courseIdentity: course, bytes: record() + Data(count: 12))
        let longer = try LegacyTintaJournal(courseIdentity: course, bytes: record() + record(uid: 2))
        XCTAssertEqual(first.overlap(with: identical), .needsConfirmation(prefixRecords: 1, identicalHistory: true))
        XCTAssertEqual(first.overlap(with: longer), .needsConfirmation(prefixRecords: 1, identicalHistory: false))
        let independent = try LegacyTintaJournal(courseIdentity: course, bytes: record(time: 101))
        XCTAssertEqual(first.overlap(with: independent), .noSharedPrefix)
        let other = try LegacyTintaJournal(courseIdentity: Data(repeating: 8, count: 16), bytes: record())
        XCTAssertEqual(first.overlap(with: other), .differentCourses)
    }
    func testInvalidRecordsAndUnknownFormatsAreRejected() throws {
        for bytes in [record(uid: 0), record(uid: UInt32.max), record(op: 5), record(op: 83),
                      record(op: 24), record(op: 16, arg: 8), record(op: 8, arg: 1)] {
            XCTAssertThrowsError(try LegacyTintaJournal(courseIdentity: course, bytes: bytes))
        }
        XCTAssertThrowsError(try LegacyTintaJournal(courseIdentity: Data(count: 16), bytes: record()))
        let empty = try LegacyTintaJournal(courseIdentity: course, bytes: Data())
        XCTAssertTrue(empty.entries.isEmpty); XCTAssertEqual(empty.effectiveReviewCount, 0)
    }
}

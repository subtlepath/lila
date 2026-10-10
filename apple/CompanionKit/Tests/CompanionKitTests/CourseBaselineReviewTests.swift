import Foundation
import XCTest
@testable import CompanionKit

final class CourseBaselineReviewTests: XCTestCase {
    private func fixture() throws -> Data {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        return try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/CourseBaselineReview-v1.fixture"))
    }
    private func seal(_ bytes: Data) -> Data {
        var sealed = Data(bytes.dropLast(4))
        sealed.appendLittleEndian(UInt64(legacyCRC32(sealed)), count: 4)
        return sealed
    }
    func testSharedReviewFixtureAndEveryTruncatedOrCorruptByte() throws {
        let bytes = try fixture(), review = try CourseBaselineReview(decoding: bytes)
        XCTAssertEqual(review.encoded, bytes)
        XCTAssertEqual(review.reader, Data(repeating: 1, count: 16))
        XCTAssertEqual(review.generation, Data(repeating: 2, count: 16))
        XCTAssertEqual(review.course, Data(repeating: 3, count: 16))
        XCTAssertEqual(review.files.count, 8)
        XCTAssertFalse(review.journalPresent)
        XCTAssertEqual(review.hash.map { String(format: "%02x", $0) }.joined(),
                       "d02419c360bc310e304f166a08cfcc28cafcfb114d514097f02fc2991b1100eb")
        XCTAssertEqual(review.files.filter { $0.domain == .journal }.map(\.name),
                       ["events.bin", "header-a.bin", "header-b.bin"])
        for length in 0..<bytes.count {
            XCTAssertThrowsError(try CourseBaselineReview(decoding: Data(bytes.prefix(length))))
        }
        for offset in bytes.indices {
            var invalid = bytes; invalid[offset] ^= 1
            XCTAssertThrowsError(try CourseBaselineReview(decoding: invalid))
        }
        XCTAssertThrowsError(try CourseBaselineReview(decoding: bytes + Data([0])))
    }
    func testResignedMalformedRosterAndPendingLearnerNamesRefuse() throws {
        let bytes = try fixture()
        let faults: [(Int, UInt8)] = [(5, 1), (6, 1), (7, 1), (58, 1), (59, 1),
                                     (60, 4), (61, 0), (62, 24), (63, 1), (64, 65), (56, 65)]
        for (offset, value) in faults {
            var invalid = bytes; invalid[offset] = value
            XCTAssertThrowsError(try CourseBaselineReview(decoding: seal(invalid)))
        }
        for name in ["items.tmp", "pack-a", "items.sync", "items.sync-old", "items.proof", "sync-intent"] {
            var invalid = bytes
            invalid[62] = UInt8(name.utf8.count)
            invalid.replaceSubrange(64..<88, with: Data(name.utf8) + Data(repeating: 0, count: 24 - name.utf8.count))
            XCTAssertThrowsError(try CourseBaselineReview(decoding: seal(invalid)))
        }
        var invalid = bytes
        let first = Data(bytes[128..<196]), second = Data(bytes[196..<264])
        invalid.replaceSubrange(128..<196, with: second)
        invalid.replaceSubrange(196..<264, with: first)
        XCTAssertThrowsError(try CourseBaselineReview(decoding: seal(invalid)))
    }
    func testConfirmationRequiresExactReaderGenerationCourseAndReviewHash() throws {
        let review = try CourseBaselineReview(decoding: fixture())
        let manifest = try ContentManifest(content: ContentID(String(repeating: "04", count: 32)),
            kind: .course, length: 4097, formatVersion: 1, logicalIdentity: review.course)
        let request = try CourseBaselineImportRequest(generation: review.generation,
            owner: Data(repeating: 5, count: 16), transaction: Data(repeating: 6, count: 16),
            manifest: manifest, reviewHash: review.hash)
        XCTAssertTrue(review.matches(request, reader: review.reader))
        XCTAssertFalse(review.matches(request, reader: Data(repeating: 9, count: 16)))
        let otherHash = try CourseBaselineImportRequest(generation: review.generation,
            owner: request.owner, transaction: request.transaction, manifest: manifest,
            reviewHash: Data(repeating: 9, count: 32))
        XCTAssertFalse(review.matches(otherHash, reader: review.reader))
        let otherGeneration = try CourseBaselineImportRequest(generation: Data(repeating: 9, count: 16),
            owner: request.owner, transaction: request.transaction, manifest: manifest, reviewHash: review.hash)
        XCTAssertFalse(review.matches(otherGeneration, reader: review.reader))
        let otherManifest = try ContentManifest(content: manifest.content, kind: .course,
            length: manifest.length, formatVersion: 1, logicalIdentity: Data(repeating: 9, count: 16))
        let otherCourse = try CourseBaselineImportRequest(generation: review.generation,
            owner: request.owner, transaction: request.transaction, manifest: otherManifest, reviewHash: review.hash)
        XCTAssertFalse(review.matches(otherCourse, reader: review.reader))
    }
}

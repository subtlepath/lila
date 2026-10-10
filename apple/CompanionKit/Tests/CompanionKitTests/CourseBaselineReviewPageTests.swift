import Foundation
import XCTest
@testable import CompanionKit

final class CourseBaselineReviewPageTests: XCTestCase {
    private func fixture(_ name: String = "CourseBaselineReview-v1.fixture") throws -> Data {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        return try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/" + name))
    }
    private func page(_ review: CourseBaselineReview, offset: Int, count: Int, hash: Data? = nil) -> Data {
        var bytes = Data([0x54, 0x43, 0x42, 0x50, 1, 0])
        bytes.appendLittleEndian(UInt64(review.encoded.count), count: 2)
        bytes.appendLittleEndian(UInt64(offset), count: 2)
        bytes.appendLittleEndian(UInt64(count), count: 2)
        bytes.append(hash ?? review.hash)
        bytes.append(review.encoded.subdata(in: offset ..< offset + count))
        bytes.appendLittleEndian(UInt64(legacyCRC32(bytes)), count: 4)
        return bytes
    }
    func testSharedNativeFixtureAndEveryTruncationOrCorruption() throws {
        let encoded = try fixture("CourseBaselineReviewPage-v1.fixture")
        let review = try CourseBaselineReview(decoding: fixture())
        let decoded = try CourseBaselineReviewPage(decoding: encoded)
        XCTAssertEqual(encoded, page(review, offset: 0, count: 97))
        XCTAssertEqual(decoded.hash, review.hash)
        XCTAssertEqual(decoded.total, review.encoded.count)
        XCTAssertEqual(decoded.offset, 0)
        for at in encoded.indices {
            XCTAssertThrowsError(try CourseBaselineReviewPage(decoding: Data(encoded.prefix(at))))
            var changed = encoded
            changed[at] ^= 1
            XCTAssertThrowsError(try CourseBaselineReviewPage(decoding: changed))
        }
    }
    func testReassemblyChecksContextAndAllowsExactDuplicates() throws {
        let review = try CourseBaselineReview(decoding: fixture())
        for limit in [1, 97, CourseBaselineReviewPage.maximumBytes] {
            var assembly = try CourseBaselineReviewAssembly(reader: review.reader, generation: review.generation,
                                                            course: review.course)
            var result: CourseBaselineReview?
            while assembly.offset < review.encoded.count {
                let offset = assembly.offset
                let encoded = page(review, offset: offset, count: min(limit, review.encoded.count - offset))
                result = try assembly.append(encoded)
                let accepted = assembly.offset
                XCTAssertEqual(try assembly.append(encoded), result)
                XCTAssertEqual(assembly.offset, accepted)
            }
            XCTAssertEqual(result, review)
        }
        for field in 0..<3 {
            var identities = [review.reader, review.generation, review.course]
            identities[field][0] ^= 0x80
            var assembly = try CourseBaselineReviewAssembly(reader: identities[0], generation: identities[1],
                                                            course: identities[2])
            XCTAssertThrowsError(try assembly.append(page(review, offset: 0, count: review.encoded.count)))
            XCTAssertEqual(assembly.offset, 0)
        }
    }
    func testChangedSkippedAndOverlappingPagesPreserveAcceptedState() throws {
        let review = try CourseBaselineReview(decoding: fixture())
        var assembly = try CourseBaselineReviewAssembly(reader: review.reader, generation: review.generation,
                                                        course: review.course)
        XCTAssertThrowsError(try assembly.append(page(review, offset: 1, count: 97)))
        XCTAssertEqual(assembly.offset, 0)
        XCTAssertNil(try assembly.append(page(review, offset: 0, count: 97)))
        XCTAssertThrowsError(try assembly.append(page(review, offset: 98, count: 97)))
        XCTAssertThrowsError(try assembly.append(page(review, offset: 96, count: 97)))
        XCTAssertThrowsError(try assembly.append(page(review, offset: 97, count: 97,
                                                     hash: Data(repeating: 9, count: 32))))
        XCTAssertEqual(assembly.offset, 97)
        var altered = page(review, offset: 0, count: 97)
        altered[44] ^= 1
        altered.removeLast(4)
        altered.appendLittleEndian(UInt64(legacyCRC32(altered)), count: 4)
        XCTAssertThrowsError(try assembly.append(altered))
        XCTAssertEqual(assembly.offset, 97)
        XCTAssertEqual(try assembly.append(page(review, offset: 97, count: review.encoded.count - 97)), review)
    }
    func testCompleteRecordRequiresActualHashNotOnlyPageChecksums() throws {
        let review = try CourseBaselineReview(decoding: fixture())
        var assembly = try CourseBaselineReviewAssembly(reader: review.reader, generation: review.generation,
                                                        course: review.course)
        XCTAssertThrowsError(try assembly.append(page(review, offset: 0, count: review.encoded.count,
                                                     hash: Data(repeating: 9, count: 32))))
        XCTAssertEqual(assembly.offset, 0)
        XCTAssertEqual(try assembly.append(page(review, offset: 0, count: review.encoded.count)), review)
    }
}

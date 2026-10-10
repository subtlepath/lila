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
    func testSharedRequestBoundsCaptureAndImmutableResume() throws {
        let encoded = try fixture("CourseBaselineReviewPageRequest-v1.fixture")
        let request = try CourseBaselineReviewPageRequest(decoding: encoded)
        XCTAssertEqual(request.encode(), encoded)
        XCTAssertEqual(request.offset, 0)
        XCTAssertEqual(request.limit, 97)
        XCTAssertEqual(request.hash, Data(repeating: 0, count: 32))
        let framed = try fixture("CourseBaselineReviewRequestFrame-v1.fixture")
        XCTAssertEqual(try request.frame(requestID: 0x12345678).encoded(), framed)
        XCTAssertEqual(Command.courseBaselineReview.rawValue, 20)
        XCTAssertThrowsError(try ControlFrame(decoding: framed, authenticated: false))
        for size in 0..<encoded.count {
            XCTAssertThrowsError(try CourseBaselineReviewPageRequest(decoding: Data(encoded.prefix(size))))
        }
        for fault in 0..<7 {
            var changed = encoded
            if fault == 0 { changed[5] = 1 }
            if fault == 1 { changed.replaceSubrange(6..<22, with: Data(repeating: 0, count: 16)) }
            if fault == 2 { changed.replaceSubrange(22..<38, with: Data(repeating: 0, count: 16)) }
            if fault == 3 { changed[70] = 1 }
            if fault == 4 { changed[70] = 0x40; changed[71] = 0x11 }
            if fault == 5 { changed[72] = 0; changed[73] = 0 }
            if fault == 6 { changed[72] = 0xd1; changed[73] = 3 }
            XCTAssertThrowsError(try CourseBaselineReviewPageRequest(decoding: changed))
        }
        let resumed = try CourseBaselineReviewPageRequest(generation: request.generation, course: request.course,
            hash: Data(repeating: 9, count: 32), offset: 97, limit: CourseBaselineReviewPage.maximumBytes)
        XCTAssertEqual(try CourseBaselineReviewPageRequest(decoding: resumed.encode()), resumed)
        XCTAssertThrowsError(try CourseBaselineReviewPageRequest(generation: request.generation, course: request.course,
                                                                offset: -1))
    }
    func testReassemblyChecksContextAndAllowsExactDuplicates() throws {
        let review = try CourseBaselineReview(decoding: fixture())
        for limit in [1, 97, CourseBaselineReviewPage.maximumBytes] {
            var assembly = try CourseBaselineReviewAssembly(reader: review.reader, generation: review.generation,
                                                            course: review.course)
            var result: CourseBaselineReview?
            while assembly.offset < review.encoded.count {
                let offset = assembly.offset
                let request = try assembly.nextRequest(limit: limit)
                XCTAssertEqual(request.offset, offset)
                XCTAssertEqual(request.generation, review.generation)
                XCTAssertEqual(request.course, review.course)
                XCTAssertEqual(request.hash, offset == 0 ? Data(repeating: 0, count: 32) : review.hash)
                let encoded = page(review, offset: offset, count: min(limit, review.encoded.count - offset))
                result = try assembly.append(encoded)
                let accepted = assembly.offset
                XCTAssertEqual(try assembly.append(encoded), result)
                XCTAssertEqual(assembly.offset, accepted)
            }
            XCTAssertEqual(result, review)
            XCTAssertThrowsError(try assembly.nextRequest())
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
    func testFramedRepliesBindRequestIDsLimitsAndFrozenHash() throws {
        let review = try CourseBaselineReview(decoding: fixture())
        let request = try CourseBaselineReviewPageRequest(generation: review.generation, course: review.course,
                                                          hash: review.hash).frame(requestID: 17)
        let payload = page(review, offset: 0, count: 97)
        let reply = try ControlFrame(command: .courseBaselineReview, response: true, requestID: 17, payload: payload)
        XCTAssertEqual(try CourseBaselineReviewPage.decode(reply, request: request).hash, review.hash)
        for changed in [
            try ControlFrame(command: .courseBaselineReview, response: false, requestID: 17, payload: payload),
            try ControlFrame(command: .courseBaselineReview, response: true, requestID: 18, payload: payload),
            try ControlFrame(command: .courseContext, response: true, requestID: 17, payload: payload),
            try ControlFrame(command: .courseBaselineReview, response: true, requestID: 17,
                             payload: page(review, offset: 1, count: 97)),
            try ControlFrame(command: .courseBaselineReview, response: true, requestID: 17,
                             payload: page(review, offset: 0, count: 96)),
            try ControlFrame(command: .courseBaselineReview, response: true, requestID: 17,
                             payload: page(review, offset: 0, count: 97, hash: Data(repeating: 9, count: 32)))
        ] {
            XCTAssertThrowsError(try CourseBaselineReviewPage.decode(changed, request: request))
        }
        for error in [CourseBaselineReviewControlError.invalidRequest, .unauthorized, .busy, .wrongStorage,
                      .unavailable, .unsupported] {
            let rejected = try ControlFrame(command: .error, response: true, requestID: 17,
                                            payload: Data([error.rawValue]))
            XCTAssertThrowsError(try CourseBaselineReviewPage.decode(rejected, request: request)) {
                XCTAssertEqual($0 as? CourseBaselineReviewControlError, error)
            }
        }
    }
}

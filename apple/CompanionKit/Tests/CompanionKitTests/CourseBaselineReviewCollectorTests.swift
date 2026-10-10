import Foundation
import XCTest
@testable import CompanionKit

private actor BaselineReviewWire: CompanionTransport {
    let review: CourseBaselineReview
    let failure: CourseBaselineReviewControlError?
    let disconnect: Bool
    private(set) var requests: [CourseBaselineReviewPageRequest] = []
    init(_ review: CourseBaselineReview, failure: CourseBaselineReviewControlError? = nil, disconnect: Bool = false) {
        self.review = review; self.failure = failure; self.disconnect = disconnect
    }
    func exchange(_ frame: ControlFrame) async throws -> ControlFrame {
        guard frame.command == .courseBaselineReview, !frame.response else { throw ProtocolError.command }
        let request = try CourseBaselineReviewPageRequest(decoding: frame.payload)
        requests.append(request)
        guard request.generation == review.generation, request.course == review.course,
              request.hash == (requests.count == 1 ? Data(repeating: 0, count: 32) : review.hash) else {
            throw ProtocolError.value
        }
        if requests.count == 2 {
            if disconnect { throw URLError(.networkConnectionLost) }
            if let failure {
                return try ControlFrame(command: .error, response: true, requestID: frame.requestID,
                                        payload: Data([failure.rawValue]))
            }
        }
        let count = min(request.limit, review.encoded.count - request.offset)
        var bytes = Data([0x54, 0x43, 0x42, 0x50, 1, 0])
        bytes.appendLittleEndian(UInt64(review.encoded.count), count: 2)
        bytes.appendLittleEndian(UInt64(request.offset), count: 2)
        bytes.appendLittleEndian(UInt64(count), count: 2)
        bytes.append(review.hash)
        bytes.append(review.encoded.subdata(in: request.offset ..< request.offset + count))
        bytes.appendLittleEndian(UInt64(legacyCRC32(bytes)), count: 4)
        return try ControlFrame(command: .courseBaselineReview, response: true, requestID: frame.requestID, payload: bytes)
    }
}

final class CourseBaselineReviewCollectorTests: XCTestCase {
    private func review() throws -> CourseBaselineReview {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        return try CourseBaselineReview(decoding: Data(contentsOf:
            root.appendingPathComponent("protocol/fixtures/CourseBaselineReview-v1.fixture")))
    }
    private func device(_ review: CourseBaselineReview, supported: Bool = true) throws -> DeviceDescriptor {
        var bytes = Data([1, 1]); bytes.append(review.reader); bytes.append(review.generation)
        bytes.append(1)
        bytes.appendLittleEndian(supported ? UInt64(ReaderCapabilities.courseTransfers.rawValue |
                                                   ReaderCapabilities.courseBaselineReviews.rawValue) : 0, count: 4)
        bytes.append(contentsOf: [80, 1, 1]); bytes.append(Data(repeating: 1, count: 32))
        return try DeviceDescriptor(decoding: bytes)
    }
    func testCollectsAuthenticatedFramesAgainstOneFrozenDigest() async throws {
        let expected = try review(), wire = BaselineReviewWire(expected)
        let collected = try await CourseBaselineReviewCollector().collect(device: device(expected), transport: wire,
                                                                          course: expected.course, pageLimit: 97)
        XCTAssertEqual(collected, expected)
        let requests = await wire.requests
        XCTAssertEqual(requests.count, 7)
        XCTAssertEqual(requests.map(\.offset), [0, 97, 194, 291, 388, 485, 582])
        XCTAssertTrue(requests.dropFirst().allSatisfy { $0.hash == expected.hash })
    }
    func testRemoteFailuresAndDisconnectDoNotPreventFreshCollection() async throws {
        let expected = try review(), collector = CourseBaselineReviewCollector()
        for error in [CourseBaselineReviewControlError.invalidRequest, .unauthorized, .busy, .wrongStorage,
                      .unavailable, .unsupported] {
            do {
                _ = try await collector.collect(device: device(expected), transport: BaselineReviewWire(expected, failure: error),
                                                course: expected.course, pageLimit: 97)
                XCTFail("Expected remote refusal")
            } catch let received { XCTAssertEqual(received as? CourseBaselineReviewControlError, error) }
        }
        do {
            _ = try await collector.collect(device: device(expected), transport: BaselineReviewWire(expected, disconnect: true),
                                            course: expected.course, pageLimit: 97)
            XCTFail("Expected disconnection")
        } catch { XCTAssertEqual((error as? URLError)?.code, .networkConnectionLost) }
        let recovered = try await collector.collect(device: device(expected), transport: BaselineReviewWire(expected),
                                                     course: expected.course)
        XCTAssertEqual(recovered, expected)
    }
    func testUnsupportedReaderSendsNoRequests() async throws {
        let expected = try review(), wire = BaselineReviewWire(expected)
        do {
            _ = try await CourseBaselineReviewCollector().collect(device: device(expected, supported: false),
                                                                  transport: wire, course: expected.course)
            XCTFail("Expected capability refusal")
        } catch { XCTAssertEqual(error as? CourseBaselineReviewControlError, .unsupported) }
        let requests = await wire.requests
        XCTAssertTrue(requests.isEmpty)
    }
}

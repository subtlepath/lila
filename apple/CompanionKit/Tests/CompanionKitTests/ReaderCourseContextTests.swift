import Foundation
import XCTest
@testable import CompanionKit

final class ReaderCourseContextTests: XCTestCase {
    private func fixture(_ name: String) throws -> Data {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        return try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/" + name))
    }
    func testSharedRequestAndRemovedReplyFixtures() throws {
        let bytes = try fixture("ReaderCourseContextRequest-v1.fixture")
        let request = try ReaderCourseContextRequest(decoding: bytes)
        XCTAssertEqual(request.encoded, bytes)
        let removed = try fixture("ReaderCourseContextRemoved-v1.fixture")
        let reply = try ReaderCourseContextReply(decoding: removed, request: request)
        XCTAssertEqual(reply.context?.source, .removed)
        XCTAssertEqual(reply.context?.manifest.kind, .course)
        XCTAssertEqual(reply.context?.manifest.length, 4097)
        XCTAssertEqual(reply.context?.manifest.content.digest, Data(repeating: 0x22, count: 32))
        XCTAssertEqual(reply.context?.manifest.logicalIdentity, Data(repeating: 0x33, count: 16))
        var live = removed; live[5] = 1
        XCTAssertEqual(try ReaderCourseContextReply(decoding: live, request: request).context?.source, .live)
    }
    func testRejectsMalformedAndTruncatedContext() throws {
        let bytes = try fixture("ReaderCourseContextRequest-v1.fixture")
        let request = try ReaderCourseContextRequest(decoding: bytes)
        for count in 0..<bytes.count {
            XCTAssertThrowsError(try ReaderCourseContextRequest(decoding: Data(bytes.prefix(count))))
        }
        let response = try fixture("ReaderCourseContextRemoved-v1.fixture")
        for count in 0..<response.count {
            XCTAssertThrowsError(try ReaderCourseContextReply(decoding: Data(response.prefix(count)), request: request))
        }
        let mutations: [(Int, UInt8)] = [(0, 0), (3, 2), (4, 255), (5, 0), (5, 3), (6, 0), (22, 2), (56, 1), (65, 2)]
        for (offset, value) in mutations {
            var changed = response; changed[offset] = value
            XCTAssertThrowsError(try ReaderCourseContextReply(decoding: changed, request: request))
        }
        for (offset, count) in [(24, 32), (57, 8), (69, 16)] {
            var changed = response; changed.replaceSubrange(offset..<(offset + count), with: Data(count: count))
            XCTAssertThrowsError(try ReaderCourseContextReply(decoding: changed, request: request))
        }
        var extended = response; extended.append(0)
        XCTAssertThrowsError(try ReaderCourseContextReply(decoding: extended, request: request))
        XCTAssertThrowsError(try ReaderCourseContextRequest(generation: Data(count: 16)))
    }
    func testContextFramesRejectForeignRequestIdentifiersAndCommands() throws {
        let request = try ReaderCourseContextRequest(generation: Data(repeating: 0x11, count: 16)).frame(requestID: 7)
        let body = try fixture("ReaderCourseContextRemoved-v1.fixture")
        let reply = try ControlFrame(command: .courseContext, response: true, requestID: 7, payload: body)
        XCTAssertEqual(try ReaderCourseContextReply.decode(reply, request: request).context?.source, .removed)
        for frame in [try ControlFrame(command: .courseContext, response: true, requestID: 8, payload: body),
                      try ControlFrame(command: .inventory, response: true, requestID: 7, payload: body),
                      try ControlFrame(command: .courseContext, requestID: 7, payload: body)] {
            XCTAssertThrowsError(try ReaderCourseContextReply.decode(frame, request: request))
        }
    }
    func testFailureRepliesHaveNoContextAndBindGeneration() throws {
        let request = try ReaderCourseContextRequest(generation: Data(repeating: 0x11, count: 16))
        for result in ReaderCourseContextResult.allCases where result != .ok {
            var generation = request.generation
            if result == .wrongStorage { generation[0] ^= 1 }
            var bytes = Data([0x4c, 0x43, 0x58, 1, result.rawValue, 0]); bytes.append(generation)
            let reply = try ReaderCourseContextReply(decoding: bytes, request: request)
            XCTAssertNil(reply.context)
            XCTAssertEqual(reply.generation, generation)
            var extra = bytes; extra.append(0)
            XCTAssertThrowsError(try ReaderCourseContextReply(decoding: extra, request: request))
            bytes[5] = 2
            XCTAssertThrowsError(try ReaderCourseContextReply(decoding: bytes, request: request))
        }
    }
}

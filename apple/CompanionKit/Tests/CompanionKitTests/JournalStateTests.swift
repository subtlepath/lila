import Foundation
import XCTest
@testable import CompanionKit

final class JournalStateTests: XCTestCase {
    private func fixture(_ name: String) throws -> Data {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        return try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/" + name))
    }
    func testSharedActiveJournalFixturesAndBothRecordSizes() throws {
        let generation = Data(repeating: 2, count: 16)
        let request = try JournalState.request(generation: generation, requestID: 42)
        XCTAssertEqual(request.payload, try fixture("JournalStateRequest-v1.fixture"))
        try JournalState.validateRequest(request.payload, generation: generation)
        let bytes = try fixture("JournalStateReply-v1.fixture")
        let reply = try ControlFrame(command: .exchangeChanges, response: true, requestID: 42, payload: bytes)
        let snapshot = try JournalState.decode(reply, request: request)
        XCTAssertEqual(snapshot.count, 0)
        XCTAssertEqual(snapshot.recordSize, 512)
        XCTAssertEqual(snapshot.frontier, try TintaJournalFrontier.digest([]))
        var extended = bytes; extended[8] = 0; extended[9] = 4
        let second = try ControlFrame(command: .exchangeChanges, response: true, requestID: 42, payload: extended)
        XCTAssertEqual(try JournalState.decode(second, request: request).recordSize, 1024)
        XCTAssertThrowsError(try JournalState.validateRequest(request.payload, generation: Data(repeating: 3, count: 16)))
        XCTAssertThrowsError(try JournalState.request(generation: Data(count: 16), requestID: 42))
    }
    func testMalformedSnapshotAndMismatchedReplyCannotBecomeADeclaration() throws {
        let request = try JournalState.request(generation: Data(repeating: 2, count: 16), requestID: 42)
        let bytes = try fixture("JournalStateReply-v1.fixture")
        func decode(_ payload: Data, id: UInt32 = 42, response: Bool = true) throws -> JournalMergeSnapshot {
            try JournalState.decode(ControlFrame(command: .exchangeChanges, response: response, requestID: id, payload: payload), request: request)
        }
        for index in [0, 1, 2, 3, 8, 10, 11] {
            var damaged = bytes; damaged[index] ^= 1
            XCTAssertThrowsError(try decode(damaged))
        }
        for count in 0..<bytes.count { XCTAssertThrowsError(try decode(bytes.prefix(count))) }
        XCTAssertThrowsError(try decode(bytes + Data([0])))
        XCTAssertThrowsError(try decode(bytes, id: 43))
        XCTAssertThrowsError(try decode(bytes, response: false))
        var zero = bytes; zero.replaceSubrange(12..<44, with: Data(count: 32))
        XCTAssertThrowsError(try decode(zero))
        var oversized = bytes; oversized.replaceSubrange(4..<8, with: Data(repeating: 255, count: 4))
        XCTAssertThrowsError(try decode(oversized))
    }
}

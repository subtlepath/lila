import Foundation
import XCTest
@testable import CompanionKit

final class JournalMergeReadinessTests: XCTestCase {
    private func fixture(_ name: String) throws -> Data {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        return try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/" + name))
    }
    func testRequestLayoutAndEveryReadinessOutcome() throws {
        let snapshot = try JournalMergeSnapshot(count: 0, recordSize: 512, frontier: TintaJournalFrontier.digest([]))
        let request = try JournalMergeReadiness.request(generation: Data(repeating: 2, count: 16), snapshot: snapshot, requestID: 42)
        var expected = Data([0x4a, 0x52, 0x44, 1]) + Data(repeating: 2, count: 16)
        expected.append(contentsOf: [0, 0, 0, 0, 0, 2, 0, 0])
        expected.append(try TintaJournalFrontier.digest([]))
        XCTAssertEqual(expected, try fixture("JournalMergeReadinessRequest-v1.fixture"))
        XCTAssertEqual(try fixture("JournalMergeReadinessReady-v1.fixture"), Data([0x4a, 0x52, 0x52, 1, 0, 0, 0, 0]))
        XCTAssertEqual(request.payload, expected)
        for result in [JournalMergeReadiness.ready, .migrationRequired, .unavailable, .noCourse] {
            let reply = try ControlFrame(command: .exchangeChanges, response: true, requestID: 42,
                payload: Data([0x4a, 0x52, 0x52, 1, result.rawValue, 0, 0, 0]))
            XCTAssertEqual(try JournalMergeReadiness.decode(reply, request: request), result)
        }
    }
    func testMalformedRepliesAndForeignRequestIDsAreRefused() throws {
        let snapshot = try JournalMergeSnapshot(count: 0, recordSize: 512, frontier: Data(repeating: 7, count: 32))
        let request = try JournalMergeReadiness.request(generation: Data(repeating: 2, count: 16), snapshot: snapshot, requestID: 42)
        let valid = Data([0x4a, 0x52, 0x52, 1, 0, 0, 0, 0])
        for offset in [0, 1, 2, 3, 4, 5, 6, 7] {
            var invalid = valid; invalid[offset] = 255
            let reply = try ControlFrame(command: .exchangeChanges, response: true, requestID: 42, payload: invalid)
            XCTAssertThrowsError(try JournalMergeReadiness.decode(reply, request: request))
        }
        let foreign = try ControlFrame(command: .exchangeChanges, response: true, requestID: 43, payload: valid)
        XCTAssertThrowsError(try JournalMergeReadiness.decode(foreign, request: request))
    }
}

import Foundation
import XCTest
@testable import CompanionKit

final class JournalMergeRequestTests: XCTestCase {
    private var fixtures: URL {
        URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("protocol/fixtures")
    }
    private func declaration() throws -> JournalMergeDeclaration {
        try JournalMergeDeclaration(generation: Data(repeating: 1, count: 16), transaction: Data(repeating: 2, count: 16),
            owner: Data(repeating: 3, count: 16),
            previous: JournalMergeSnapshot(count: 1, recordSize: 512, frontier: Data(repeating: 4, count: 32)),
            merged: JournalMergeSnapshot(count: 3, recordSize: 1024, frontier: Data(repeating: 5, count: 32)))
    }
    func testDeclarationAndOperationsMatchSharedFixture() throws {
        let declaration = try declaration()
        let fixture = try Data(contentsOf: fixtures.appendingPathComponent("JournalMergeBegin-v1.fixture"))
        XCTAssertEqual(try JournalMergeRequest.begin(declaration).payload(), fixture)
        XCTAssertEqual(try JournalMergeRequest.decodePayload(fixture), .begin(declaration))
        for offset in [0, 3, 5, 8, 24, fixture.count - 1] {
            var corrupt = fixture; corrupt[offset] ^= 1
            XCTAssertThrowsError(try JournalMergeRequest.decodePayload(corrupt))
        }
        for (operation, request) in [(UInt8(3), JournalMergeRequest.commit(declaration)),
                                      (UInt8(4), JournalMergeRequest.abort(declaration))] {
            var expected = fixture; expected[4] = operation
            XCTAssertEqual(try request.payload(), expected)
        }
        let frame = try JournalMergeRequest.begin(declaration).frame(requestID: 42)
        XCTAssertEqual(frame.command, .exchangeChanges)
        XCTAssertEqual(frame.requestID, 42)
        XCTAssertEqual(frame.payload, fixture)
    }
    func testAppendMatchesSharedFixtureAndRejectsZeroTransaction() throws {
        let page = try JournalExportPage(decoding: Data(contentsOf: fixtures.appendingPathComponent("JournalExportPage-v1.fixture")), requested: .start)
        let mutation = try XCTUnwrap(page.mutation)
        let request = JournalMergeRequest.append(transaction: Data(repeating: 2, count: 16), mutation: mutation)
        XCTAssertEqual(try request.payload(), try Data(contentsOf: fixtures.appendingPathComponent("JournalMergeAppend-v1.fixture")))
        XCTAssertEqual(try JournalMergeRequest.decodePayload(request.payload()), request)
        XCTAssertThrowsError(try JournalMergeRequest.append(transaction: Data(count: 16), mutation: mutation).payload())
    }
    func testSnapshotAndDeclarationConstraints() throws {
        XCTAssertThrowsError(try JournalMergeSnapshot(count: 1, recordSize: 256, frontier: Data(repeating: 1, count: 32)))
        XCTAssertThrowsError(try JournalMergeSnapshot(count: UInt32.max, recordSize: 1024, frontier: Data(repeating: 1, count: 32)))
        XCTAssertThrowsError(try JournalMergeSnapshot(count: 0, recordSize: 1024, frontier: Data(count: 32)))
        let original = try declaration()
        let mismatched = try JournalMergeSnapshot(count: 1, recordSize: 1024, frontier: original.merged.frontier)
        XCTAssertThrowsError(try JournalMergeDeclaration(generation: original.generation, transaction: original.transaction,
            owner: original.owner, previous: original.previous, merged: mismatched))
    }
    func testReplyRequiresMatchingRequestTransactionAndValidStatus() throws {
        let request = try JournalMergeRequest.begin(declaration()).frame(requestID: 42)
        var payload = Data([1, 0, 0, 0]) + Data(repeating: 2, count: 16)
        payload.appendLittleEndian(1, count: 4)
        let reply = try ControlFrame(command: .exchangeChanges, response: true, requestID: 42, payload: payload)
        XCTAssertEqual(try JournalMergeReply.decode(reply, request: request).count, 1)
        for offset in [0, 1, 2, 4] {
            var corrupt = payload; corrupt[offset] = 255
            XCTAssertThrowsError(try JournalMergeReply.decode(ControlFrame(command: .exchangeChanges, response: true,
                requestID: 42, payload: corrupt), request: request))
        }
        XCTAssertThrowsError(try JournalMergeReply.decode(ControlFrame(command: .exchangeChanges, response: true,
            requestID: 43, payload: payload), request: request))
    }
}

import Foundation
import XCTest
@testable import CompanionKit

final class TintaMigrationAdmissionTests: XCTestCase {
    private func admission() throws -> TintaMigrationAdmission {
        let merge = try JournalMergeDeclaration(generation: Data(repeating: 1, count: 16),
            transaction: Data(repeating: 3, count: 16), owner: Data(repeating: 2, count: 16),
            previous: JournalMergeSnapshot(count: 0, recordSize: 512, frontier: Data(repeating: 4, count: 32)),
            merged: JournalMergeSnapshot(count: 1, recordSize: 1024, frontier: Data(repeating: 5, count: 32)))
        return try TintaMigrationAdmission(merge: merge, course: Data(repeating: 6, count: 16),
            resource: Data(repeating: 7, count: 32), backupTransaction: Data(repeating: 8, count: 16),
            reader: Data(repeating: 9, count: 16), backupManifest: Data(repeating: 10, count: 32))
    }
    func testSharedFixtureAndEveryCorruptionAndTruncation() throws {
        let fixtureURL = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("protocol/fixtures/TintaMigrationAdmission-v1.fixture")
        let fixture = try Data(contentsOf: fixtureURL), expected = try admission()
        XCTAssertEqual(expected.bytes, fixture)
        XCTAssertEqual(try TintaMigrationAdmission(decoding: fixture), expected)
        for index in fixture.indices {
            var corrupt = fixture; corrupt[index] ^= 1
            XCTAssertThrowsError(try TintaMigrationAdmission(decoding: corrupt))
            XCTAssertThrowsError(try TintaMigrationAdmission(decoding: fixture.prefix(index)))
        }
    }
    func testValidChecksumCannotAdmitMissingBindingsOrNoNewHistory() throws {
        let expected = try admission()
        for offset in [140, 156, 188, 204, 220] {
            var damaged = expected.bytes
            let length = offset == 156 || offset == 220 ? 32 : 16
            damaged.replaceSubrange(offset ..< offset + length, with: Data(count: length))
            damaged.removeLast(4)
            damaged.appendLittleEndian(UInt64(legacyCRC32(damaged)), count: 4)
            XCTAssertThrowsError(try TintaMigrationAdmission(decoding: damaged))
        }
        let noChange = try JournalMergeDeclaration(generation: expected.merge.generation,
            transaction: expected.merge.transaction, owner: expected.merge.owner,
            previous: expected.merge.merged, merged: expected.merge.merged)
        XCTAssertThrowsError(try TintaMigrationAdmission(merge: noChange, course: expected.course,
            resource: expected.resource, backupTransaction: expected.backupTransaction,
            reader: expected.reader, backupManifest: expected.backupManifest))
    }
    func testAdmissionFrameAndReplyBindRequestTransactionAndOldCount() throws {
        let value = try admission(), request = try value.frame(requestID: 42)
        XCTAssertEqual(request.command, .exchangeChanges)
        XCTAssertEqual(request.payload, value.bytes)
        var payload = Data([1, 0, 0, 0])
        payload.append(value.merge.transaction)
        payload.appendLittleEndian(UInt64(value.merge.previous.count), count: 4)
        func reply(_ bytes: Data, id: UInt32 = 42, response: Bool = true) throws -> ControlFrame {
            try ControlFrame(command: .exchangeChanges, response: response, requestID: id, payload: bytes)
        }
        let decoded = try TintaMigrationAdmissionReply.decode(reply(payload), request: request)
        XCTAssertEqual(decoded.result, .ok)
        XCTAssertEqual(decoded.count, value.merge.previous.count)
        for index in payload.indices where index != 1 {
            var corrupt = payload; corrupt[index] ^= 1
            XCTAssertThrowsError(try TintaMigrationAdmissionReply.decode(reply(corrupt), request: request))
        }
        for result in UInt8(0)...UInt8(7) {
            var outcome = payload; outcome[1] = result
            XCTAssertEqual(try TintaMigrationAdmissionReply.decode(reply(outcome), request: request).result.rawValue, result)
        }
        var unknown = payload; unknown[1] = 255
        XCTAssertThrowsError(try TintaMigrationAdmissionReply.decode(reply(unknown), request: request))
        XCTAssertThrowsError(try TintaMigrationAdmissionReply.decode(reply(payload, id: 43), request: request))
        XCTAssertThrowsError(try TintaMigrationAdmissionReply.decode(reply(payload, response: false), request: request))
        XCTAssertThrowsError(try TintaMigrationAdmissionReply.decode(reply(payload.dropLast()), request: request))
    }
}

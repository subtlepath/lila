import Foundation
import XCTest
@testable import CompanionKit

final class LegacyBackupRequestTests: XCTestCase {
    func testSharedFixtureAndBounds() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let bytes = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/LegacyBackupFileRequest-v1.fixture"))
        let request = try LegacyBackupRequest(decoding: bytes)
        XCTAssertEqual(request.bytes, bytes)
        XCTAssertEqual(request.role, .reviews)
        XCTAssertEqual(request.offset, 0x12345678)
        XCTAssertEqual(request.count, 768)
        for offset in [3, 4, 5, 6, 7, 62, 63] {
            var invalid = bytes; invalid[offset] = 0xfe
            XCTAssertThrowsError(try LegacyBackupRequest(decoding: invalid))
        }
        XCTAssertThrowsError(try LegacyBackupRequest(decoding: bytes.dropLast()))
        XCTAssertThrowsError(try LegacyBackupRequest(operation: .file, role: .reviews, bound: true,
            course: request.course, transaction: request.transaction, generation: request.generation, count: 769))
        let manifest = try LegacyBackupRequest(operation: .manifest, bound: true,
            course: request.course, transaction: request.transaction, generation: request.generation)
        XCTAssertEqual(try LegacyBackupRequest(decoding: manifest.bytes), manifest)
    }
}

extension LegacyBackupRequestTests {
    func testSharedReplyRejectsForeignTransactionAndWrongOffset() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let bytes = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/LegacyBackupFileReply-v1.fixture"))
        func identity(_ value: UInt8) -> Data { Data([value]) + Data(repeating: 0, count: 15) }
        let request = try LegacyBackupRequest(operation: .file, role: .reviews, bound: true,
            course: identity(1), transaction: identity(2), generation: identity(3), offset: 31, count: 768)
        let reply = try LegacyBackupReply(decoding: bytes, request: request)
        XCTAssertEqual(reply.body.count, 768)
        XCTAssertEqual(reply.result, .ok)
        XCTAssertFalse(reply.final)
        for offset in [4, 5, 6, 7, 8, 24, 28, 30, 31] {
            var invalid = bytes; invalid[offset] = 0xfe
            XCTAssertThrowsError(try LegacyBackupReply(decoding: invalid, request: request))
        }
        XCTAssertThrowsError(try LegacyBackupReply(decoding: bytes.dropLast(), request: request))
    }
}

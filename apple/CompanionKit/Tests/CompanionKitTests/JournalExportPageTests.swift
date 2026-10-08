import Foundation
import XCTest
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

final class JournalExportPageTests: XCTestCase {
    private func page(count: UInt32 = 1, next: UInt32 = 1, complete: Bool = false) throws -> Data {
        let body = Data([1, 9]) + Data(repeating: 7, count: 16) + Data([1, 0, 0, 0, 1])
        let identity = try EventIdentity(origin: Data(repeating: 1, count: 16), epoch: 1, sequence: 1)
        let event = try SyncEvent(identity: identity, storageGeneration: Data(repeating: 2, count: 16),
                                  kind: .star, resource: Data(repeating: 3, count: 32), bodyHash: Data(SHA256.hash(data: body)))
        var bytes = Data([1, complete ? 1 : 0, 0, 0])
        bytes.appendLittleEndian(UInt64(count), count: 4)
        bytes.appendLittleEndian(UInt64(next), count: 4)
        bytes.append(Data(repeating: 9, count: 32))
        bytes.appendLittleEndian(complete ? 0 : UInt64(event.bytes.count), count: 2)
        bytes.appendLittleEndian(complete ? 0 : UInt64(body.count), count: 2)
        if !complete { bytes.append(event.bytes); bytes.append(body) }
        return bytes
    }
    func testPageRetryCursorAndExplicitEnd() throws {
        XCTAssertEqual(JournalExportCursor.start.payload, Data(count: 40))
        let bytes = try page()
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        XCTAssertEqual(bytes, try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/JournalExportPage-v1.fixture")))
        let first = try JournalExportPage(decoding: bytes, requested: .start)
        XCTAssertFalse(first.complete)
        XCTAssertEqual(first.mutation?.event.kind, .star)
        XCTAssertEqual(first.cursor.payload.count, 40)
        XCTAssertEqual(try JournalExportPage(decoding: bytes, requested: .start), first)
        let end = try JournalExportPage(decoding: page(complete: true), requested: first.cursor)
        XCTAssertTrue(end.complete)
        XCTAssertEqual(end.cursor, first.cursor)
        XCTAssertThrowsError(try JournalExportPage(decoding: page(complete: true), requested: .start))
        XCTAssertThrowsError(try JournalExportPage(decoding: bytes, requested: first.cursor))
    }
    func testEmptyAndMalformedPages() throws {
        XCTAssertTrue(try JournalExportPage(decoding: page(count: 0, next: 0, complete: true), requested: .start).complete)
        let bytes = try page()
        for offset in [0, 1, 2, 3, 44, 46, bytes.count - 1] {
            var corrupt = bytes; corrupt[offset] ^= 0xff
            XCTAssertThrowsError(try JournalExportPage(decoding: corrupt, requested: .start))
        }
        for length in [0, 47, bytes.count - 1] {
            XCTAssertThrowsError(try JournalExportPage(decoding: bytes.prefix(length), requested: .start))
        }
        let cursor = try JournalExportPage(decoding: bytes, requested: .start).cursor
        var changed = try page(complete: true); changed[12] ^= 1
        XCTAssertThrowsError(try JournalExportPage(decoding: changed, requested: cursor)) {
            XCTAssertEqual($0 as? HistoryError, .staleFrontier)
        }
    }
}

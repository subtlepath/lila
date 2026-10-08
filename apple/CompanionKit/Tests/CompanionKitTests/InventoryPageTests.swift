import Foundation
import XCTest
@testable import CompanionKit

final class InventoryPageTests: XCTestCase {
    private func page(cursor: UInt64 = 0, revision: UInt64 = 1, complete: Bool, hash: UInt8) throws -> InventoryPage {
        var bytes = Data([1]); bytes.append(Data(repeating: 2, count: 16))
        bytes.appendLittleEndian(revision, count: 8); bytes.appendLittleEndian(cursor, count: 8)
        bytes.appendLittleEndian(complete ? 0 : cursor + 1, count: 8); bytes.append(complete ? 1 : 0); bytes.append(1)
        bytes.append(contentsOf: [1, 2]); bytes.append(Data(repeating: hash, count: 32)); bytes.append(1)
        bytes.appendLittleEndian(3, count: 8); bytes.appendLittleEndian(1, count: 4); bytes.append(Data(count: 16))
        return try InventoryPage(decoding: bytes)
    }
    func testOnlyCompleteConsistentScanCanReconcile() throws {
        var scan = try InventoryScan(reader: Data(repeating: 1, count: 16), generation: Data(repeating: 2, count: 16), maximumEntries: 2)
        XCTAssertEqual(try scan.nextRequest(requestID: 1).payload.count, 34)
        XCTAssertThrowsError(try scan.inventory())
        try scan.append(page(complete: false, hash: 3))
        XCTAssertThrowsError(try scan.append(page(cursor: 1, revision: 2, complete: true, hash: 4)))
        XCTAssertEqual(scan.cursor, 1)
        XCTAssertThrowsError(try scan.append(page(cursor: 1, complete: true, hash: 3)))
        try scan.append(page(cursor: 1, complete: true, hash: 4))
        XCTAssertEqual(try scan.inventory().contents.count, 2)
        XCTAssertThrowsError(try scan.nextRequest(requestID: 2))
    }
    func testSharedFixture() throws {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf: root.appendingPathComponent("protocol/fixtures/InventoryPage.json"))) as! [String: Any]
        let hex = try XCTUnwrap(json["binaryHex"] as? String)
        var bytes = Data(); bytes.reserveCapacity(hex.count / 2)
        for offset in stride(from: 0, to: hex.count, by: 2) {
            let start = hex.index(hex.startIndex, offsetBy: offset)
            bytes.append(try XCTUnwrap(UInt8(hex[start..<hex.index(start, offsetBy: 2)], radix: 16)))
        }
        let decoded = try InventoryPage(decoding: bytes)
        XCTAssertEqual(decoded.revision, 1); XCTAssertTrue(decoded.complete)
        XCTAssertEqual(decoded.contents.first?.kind, .course)
        XCTAssertEqual(decoded.contents.first?.length, 123456)
    }
    func testBoundsWrongCursorAndMalformedPage() throws {
        var scan = try InventoryScan(reader: Data(repeating: 1, count: 16), generation: Data(repeating: 2, count: 16), maximumEntries: 0)
        XCTAssertThrowsError(try scan.append(page(complete: true, hash: 3)))
        XCTAssertFalse(scan.complete)
        XCTAssertThrowsError(try InventoryPage(decoding: Data(count: 43)))
        XCTAssertThrowsError(try InventoryPage.request(generation: Data(repeating: 2, count: 16), revision: 0, cursor: 1, requestID: 1))
    }
}

import Foundation
import XCTest
@testable import CompanionKit

final class GzipTests: XCTestCase {
    private func fixture() throws -> Data {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { root.deleteLastPathComponent() }
        return try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/gzip-expansion.gz"))
    }
    func testEveryFragmentBoundaryAndMultipleOutputWindows() throws {
        let data = try fixture()
        for split in 0 ... data.count {
            let validator = try GzipValidator(limit: 120_000)
            try validator.consume(data.prefix(split)); try validator.consume(data.dropFirst(split))
            XCTAssertEqual(try validator.finish(), 120_000)
        }
    }
    func testRejectsCorruptionTruncationAndTrailingStreams() throws {
        let data = try fixture()
        var corrupt = data; corrupt[corrupt.count - 8] ^= 1
        for bytes in [corrupt, Data(data.dropLast()), data + data, data + Data([0])] {
            let validator = try GzipValidator(limit: 240_000)
            XCTAssertThrowsError(try { try validator.consume(bytes); _ = try validator.finish() }())
        }
    }
    func testExpandedSizeLimitStopsInflation() throws {
        let validator = try GzipValidator(limit: 119_999)
        XCTAssertThrowsError(try validator.consume(fixture())) { error in
            XCTAssertEqual(error as? ImportError, .resourceLimit)
        }
    }
}

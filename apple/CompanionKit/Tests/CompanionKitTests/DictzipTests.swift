import Foundation
import XCTest
@testable import CompanionKit

final class DictzipTests: XCTestCase {
    private func fixture() -> URL {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { root.deleteLastPathComponent() }
        return root.appendingPathComponent("protocol/fixtures/dictzip-chunks.dict.dz")
    }
    func testIndependentChunksAndShortFinalChunk() throws {
        let metadata = try DictzipInspector.inspect(fixture())
        XCTAssertEqual(metadata.expandedBytes, 120_000)
        XCTAssertEqual(metadata.chunkLength, 58_315)
        XCTAssertEqual(metadata.chunks, 3)
    }
    func testRejectsIncorrectRandomAccessTableDespiteValidGzip() throws {
        let original = try Data(contentsOf: fixture())
        var lengths = original
        lengths[22] += 1; lengths[24] -= 1
        var version = original; version[16] = 2
        var chunkWidth = original; chunkWidth[18] = 1; chunkWidth[19] = 0
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        for bytes in [lengths, version, chunkWidth] {
            try bytes.write(to: url)
            XCTAssertEqual(try GzipInspector.inspect(url), 120_000)
            XCTAssertThrowsError(try DictzipInspector.inspect(url))
        }
    }
}

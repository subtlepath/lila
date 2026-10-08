import Foundation
import XCTest
import ZIPFoundation
@testable import CompanionKit

final class ZipHeaderNameTests: XCTestCase {
    func testEveryByteMatchesSharedReaderMapping() throws {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { root.deleteLastPathComponent() }
        let expected = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/ZipName-cp437-utf8.fixture"))
        let decoded = try ZipHeaderName.decode(Data((0 ... 255).map(UInt8.init)), utf8: false)
        XCTAssertEqual(Data(decoded.utf8), expected)
    }
    func testStrictUtf8AndDecodedLimit() throws {
        XCTAssertEqual(try ZipHeaderName.decode(Data([0x63, 0x82]), utf8: false), "cé")
        XCTAssertThrowsError(try ZipHeaderName.decode(Data([0xc0, 0xaf]), utf8: true))
        XCTAssertThrowsError(try ZipHeaderName.decode(Data(repeating: 0xb3, count: 342), utf8: false))
        XCTAssertEqual(try ZipHeaderName.decode(Data(repeating: 0xb3, count: 341), utf8: false).utf8.count, 1023)
        XCTAssertThrowsError(try ZipHeaderName.decode(Data(), utf8: true))
    }
    func testControlsAndTraversalReachSamePathRejection() throws {
        for bytes in [Data([97, 0]), Data([97, 31]), Data([97, 127]), Data("../a".utf8)] {
            let decoded = try ZipHeaderName.decode(bytes, utf8: false)
            XCTAssertThrowsError(try ArchiveValidator.canonicalPath(decoded, directory: false))
        }
    }
    private func rawLegacyArchive(_ byte: UInt8) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".zip")
        do {
            let archive = try Archive(url: url, accessMode: .create)
            try archive.addEntry(with: "aa", type: .file, uncompressedSize: Int64(0)) { _, _ in Data() }
        }
        var data = try Data(contentsOf: url)
        let central = try XCTUnwrap((0 ..< data.count - 4).first { at in
            Array(data[at ..< at + 4]) == [0x50, 0x4b, 0x01, 0x02]
        })
        data[7] &= 0xf7
        data[central + 9] &= 0xf7
        data[30] = byte
        data[central + 46] = byte
        try data.write(to: url)
        return url
    }
    func testImporterUsesExplicitCentralNameRatherThanOsFallback() throws {
        let url = try rawLegacyArchive(0xf4)
        defer { try? FileManager.default.removeItem(at: url) }
        let result = try ArchiveValidator.validate(url)
        XCTAssertEqual(result.paths, ["⌠a"])
        XCTAssertNotNil(try ArchiveValidator.entries(url)["⌠a"])
    }
    func testImporterRejectsRawCp437ControlNames() throws {
        for byte: UInt8 in [1, 31, 127] {
            let url = try rawLegacyArchive(byte)
            defer { try? FileManager.default.removeItem(at: url) }
            XCTAssertThrowsError(try ArchiveValidator.validate(url)) { error in
                XCTAssertEqual(error as? ImportError, .unsafePath)
            }
        }
    }

}

import Foundation
import XCTest
@testable import CompanionKit

final class BitmapFontTests: XCTestCase, @unchecked Sendable {
    private func font() -> Data {
        var bytes = Data(repeating: 0, count: 93)
        bytes.replaceSubrange(0 ..< 8, with: [67, 80, 70, 79, 78, 84, 0, 0])
        bytes[8] = 4; bytes[12] = 1
        bytes[36] = 1; bytes[40] = 1; bytes[44] = 16; bytes[56] = 64
        bytes[64] = 65; bytes[68] = 65
        bytes[76] = 1; bytes[77] = 1; bytes[78] = 16; bytes[84] = 1; bytes[92] = 0x80
        return bytes
    }
    func testValidatedFontImportPersists() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("Demo_16.cpfont")
        try font().write(to: source)
        XCTAssertEqual(try BitmapFontInspector.inspect(source).styles, [0])
        let database = root.appendingPathComponent("library.sqlite")
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let importer = ContentImporter(vault: vault, library: try LibraryStore(url: database))
        let imported = try await importer.importFile(source)
        let reopened = try LibraryStore(url: database)
        let content = try await reopened.content(imported.id)
        XCTAssertEqual(content?.kind, .font)
        XCTAssertEqual(content?.title, "Demo_16")
    }
    func testRejectsWrongVersionTruncationIntervalsAndBitmapBounds() throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        var version = font(); version[8] = 3
        var interval = font(); interval[68] = 64
        var offset = font(); offset[88] = 2
        var dataStart = font(); dataStart[56] = 0
        var count = font(); count[12] = 5
        for bytes in [version, interval, offset, dataStart, count, Data(font().dropLast())] {
            try bytes.write(to: url)
            XCTAssertThrowsError(try BitmapFontInspector.inspect(url))
        }
    }
    func testKerningLigaturesAndDimensionChecks() throws {
        var bytes = font()
        bytes[49] = 1; bytes[51] = 1; bytes[53] = 1; bytes[54] = 1; bytes[55] = 1
        let tables = Data([65, 0, 1, 65, 0, 1, 0, 65, 0, 65, 0, 65, 0, 0, 0])
        bytes.insert(contentsOf: tables, at: 92)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        try bytes.write(to: url)
        XCTAssertEqual(try BitmapFontInspector.inspect(url).styles, [0])
        var badClass = bytes; badClass[94] = 2
        var badReplacement = bytes; badReplacement[103] = 66
        var dimensions = bytes; dimensions[76] = 9
        for invalid in [badClass, badReplacement, dimensions] {
            try invalid.write(to: url)
            XCTAssertThrowsError(try BitmapFontInspector.inspect(url))
        }
    }
    func testSharedFirmwareFontFixture() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let fixture = root.appendingPathComponent("protocol/fixtures/BitmapFont-v4.fixture")
        XCTAssertEqual(try BitmapFontInspector.inspect(fixture).version, 4)
        XCTAssertEqual(try BitmapFontInspector.inspect(fixture).styles, [0])
        let temporary = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: temporary) }
        let bytes = try Data(contentsOf: fixture)
        for position in [0, 8, 12, 32, 126, 135] {
            var invalid = bytes
            invalid[position] = 255
            try invalid.write(to: temporary)
            XCTAssertThrowsError(try BitmapFontInspector.inspect(temporary))
        }
    }
}

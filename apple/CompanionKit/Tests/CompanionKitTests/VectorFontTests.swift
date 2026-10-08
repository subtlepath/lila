import Foundation
import XCTest
@testable import CompanionKit

final class VectorFontTests: XCTestCase, @unchecked Sendable {
    func testRepositorySourceFonts() throws {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { root.deleteLastPathComponent() }
        for name in ["NotoSerif/NotoSerif-Bold.ttf", "NotoSansArabic/NotoSansArabic-Regular.ttf", "Ubuntu/Ubuntu-Regular.ttf"] {
            let url = root.appendingPathComponent("lib/EpdFont/builtinFonts/source/" + name)
            XCTAssertEqual(try VectorFontInspector.inspect(url).faces, 1)
        }
    }
    private func put(_ value: UInt64, at: Int, width: Int = 4, in bytes: inout Data) {
        for index in 0 ..< width { bytes[at + index] = UInt8(truncatingIfNeeded: value >> ((width - index - 1) * 8)) }
    }
    private func font(cff: Bool = false, collection: Bool = false) -> Data {
        let tables: [(String, Int)] = cff
            ? [("CFF ", 4), ("cmap", 4), ("head", 54), ("hhea", 36), ("hmtx", 4), ("maxp", 6), ("name", 6)]
            : [("cmap", 4), ("glyf", 4), ("head", 54), ("hhea", 36), ("hmtx", 4), ("loca", 4), ("maxp", 6), ("name", 6)]
        let face = collection ? 16 : 0
        var bytes = Data(repeating: 0, count: face + 12 + tables.count * 16)
        if collection {
            put(0x74746366, at: 0, in: &bytes); put(0x00010000, at: 4, in: &bytes)
            put(1, at: 8, in: &bytes); put(16, at: 12, in: &bytes)
        }
        put(cff ? 0x4f54544f : 0x00010000, at: face, in: &bytes)
        put(UInt64(tables.count), at: face + 4, width: 2, in: &bytes)
        for (index, table) in tables.enumerated() {
            let at = face + 12 + index * 16
            bytes.replaceSubrange(at ..< at + 4, with: table.0.utf8)
            put(UInt64(bytes.count), at: at + 8, in: &bytes)
            put(UInt64(table.1), at: at + 12, in: &bytes)
            bytes.append(Data(repeating: 0, count: (table.1 + 3) / 4 * 4))
        }
        return bytes
    }
    func testTrueTypeCFFAndCollectionDirectories() throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        for bytes in [font(), font(cff: true), font(collection: true)] {
            try bytes.write(to: url)
            XCTAssertEqual(try VectorFontInspector.inspect(url).faces, 1)
        }
    }
    func testRejectsChecksumsDuplicatesBoundsAndCollectionOffsets() throws {
        var checksum = font(); checksum[checksum.count - 1] = 1
        var duplicate = font(); duplicate.replaceSubrange(28 ..< 32, with: duplicate[12 ..< 16])
        var bounds = font(); put(UInt64.max, at: 20, in: &bounds)
        var collection = font(collection: true); put(17, at: 12, in: &collection)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        // Corrupt a byte covered by the final table's declared length.
        checksum[checksum.count - 4] = 1
        for bytes in [checksum, duplicate, bounds, collection, Data(font().prefix(11))] {
            try bytes.write(to: url)
            XCTAssertThrowsError(try VectorFontInspector.inspect(url))
        }
    }
    func testImportPersistsFontKind() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("Example.ttf")
        try font().write(to: source)
        let database = root.appendingPathComponent("library.sqlite")
        let importer = ContentImporter(vault: try ContentVault(root: root.appendingPathComponent("vault")),
                                       library: try LibraryStore(url: database))
        let imported = try await importer.importVectorFont(source)
        let reopened = try LibraryStore(url: database)
        let content = try await reopened.content(imported.id)
        XCTAssertEqual(content?.kind, .font)
    }
    func testSharedFirmwareVectorFixtures() throws {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { root.deleteLastPathComponent() }
        for (name, faces) in [("VectorFont-sfnt.fixture", 1), ("VectorFont-otto.fixture", 1), ("VectorFont-collection.fixture", 2)] {
            XCTAssertEqual(try VectorFontInspector.inspect(root.appendingPathComponent("protocol/fixtures/" + name)).faces, faces)
        }
    }
}

import Foundation
import XCTest
@testable import CompanionKit

final class ImportRoutingTests: XCTestCase, @unchecked Sendable {
    func testCourseDispatchPersistsDetailsAndRejectsRemoteOrUnsupportedInput() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { workspace.deleteLastPathComponent() }
        let source = root.appendingPathComponent("Spanish.PACK")
        try FileManager.default.copyItem(at: workspace.appendingPathComponent("test/tinta/fixtures/mini.pack"), to: source)
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let importer = ContentImporter(vault: try ContentVault(root: root.appendingPathComponent("vault")), library: library)
        let imported = try await importer.importFile(source)
        XCTAssertEqual(imported.kind, .course)
        let details = try await library.coursePackDetails(imported.id)
        XCTAssertNotNil(details)
        for url in [URL(string: "https://example.com/course.pack")!, root.appendingPathComponent("firmware.bin"),
                    root.appendingPathComponent("course.pack.tmp")] {
            do {
                _ = try await importer.importFile(url)
                XCTFail("unsupported inputs must not be imported")
            } catch let error as ImportError { XCTAssertEqual(error, .unsupportedEntry) }
        }
        let disguised = root.appendingPathComponent("course.epub")
        try FileManager.default.copyItem(at: source, to: disguised)
        do {
            _ = try await importer.importFile(disguised)
            XCTFail("extension routing must still validate the file contents")
        } catch { }
        let ids = try await library.libraryContentIDs()
        XCTAssertEqual(ids, [imported.id])
    }
    func testFontCollectionRoutingPreservesBytesAndCloudDescriptor() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        var repository = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { repository.deleteLastPathComponent() }
        let source = root.appendingPathComponent("Collection.ttc")
        try FileManager.default.copyItem(at: repository.appendingPathComponent("protocol/fixtures/VectorFont-collection.fixture"), to: source)
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let content = try await ContentImporter(vault: vault, library: library).importFile(source)
        XCTAssertEqual(content.kind, .font); XCTAssertEqual(content.originalFilename, "Collection.ttc")
        let stored = try await vault.verifiedObject(content.id)
        XCTAssertEqual(try Data(contentsOf: stored.url), try Data(contentsOf: source))
        let descriptor = try CloudContentDescriptor(content: content)
        XCTAssertEqual(descriptor.originalFilename, "Collection.ttc")
        let retained = try await library.content(content.id); XCTAssertEqual(retained, content)
    }

}

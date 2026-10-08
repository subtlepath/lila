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
}

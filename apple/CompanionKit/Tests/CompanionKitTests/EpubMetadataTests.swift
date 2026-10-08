import Foundation
import XCTest
import ZIPFoundation
@testable import CompanionKit

final class EpubMetadataTests: XCTestCase, @unchecked Sendable {
    private let container = """
        <container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>
        <rootfile full-path="OEBPS/package.opf" media-type="application/oebps-package+xml"/>
        </rootfiles></container>
        """
    private let package = """
        <package xmlns="http://www.idpf.org/2007/opf" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
        <dc:title> Café &amp; Books </dc:title><dc:creator>Ana</dc:creator><dc:creator>José</dc:creator>
        <dc:identifier>urn:uuid:book</dc:identifier><dc:language>es</dc:language></metadata>
        <manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>
        <spine><itemref idref="chapter"/></spine></package>
        """
    private func epub(container: String? = nil, package: String? = nil, mimetype: String = "application/epub+zip",
                      packagePath: String = "OEBPS/package.opf", chapterPath: String = "OEBPS/chapter.xhtml") throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".epub")
        let archive = try Archive(url: url, accessMode: .create)
        for (path, text) in [("mimetype", mimetype), ("META-INF/container.xml", container ?? self.container),
                             (packagePath, package ?? self.package),
                             (chapterPath, "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>Book</body></html>")] {
            let bytes = Data(text.utf8)
            try archive.addEntry(with: path, type: .file, uncompressedSize: Int64(bytes.count)) { offset, size in
                bytes.subdata(in: Int(offset) ..< min(bytes.count, Int(offset) + size))
            }
        }
        return url
    }
    func testNamespaceAwareMetadataAndUnicode() throws {
        let url = try epub()
        defer { try? FileManager.default.removeItem(at: url) }
        let metadata = try EpubInspector.inspect(url)
        XCTAssertEqual(metadata.title, "Café & Books")
        XCTAssertEqual(metadata.authors, ["Ana", "José"])
        XCTAssertEqual(metadata.identifiers, ["urn:uuid:book"])
        XCTAssertEqual(metadata.languages, ["es"])
        XCTAssertEqual(metadata.packagePath, "OEBPS/package.opf")
    }
    func testRejectsWrongMimetypeNamespaceAndMissingTitle() throws {
        for url in [try epub(mimetype: "application/zip"),
                    try epub(package: package.replacingOccurrences(of: "http://purl.org/dc/elements/1.1/", with: "urn:wrong")),
                    try epub(package: "<broken>")] {
            defer { try? FileManager.default.removeItem(at: url) }
            XCTAssertThrowsError(try EpubInspector.inspect(url))
        }
    }
    func testRejectsUnsafeRootfileAndEntities() throws {
        for url in [try epub(container: container.replacingOccurrences(of: "OEBPS/package.opf", with: "../package.opf")),
                    try epub(package: "<!DOCTYPE package [<!ENTITY secret 'injected'>]>" + package),
                    try epub(package: "<!DOCTYPE package [<!ENTITY secret SYSTEM 'file:///etc/passwd'>]>" + package)] {
            defer { try? FileManager.default.removeItem(at: url) }
            XCTAssertThrowsError(try EpubInspector.inspect(url))
        }
    }
    func testManifestAndSpineRejectMissingResourcesAndReferences() throws {
        for modified in [package.replacingOccurrences(of: "href=\"chapter.xhtml\"", with: "href=\"missing.xhtml\""),
                         package.replacingOccurrences(of: "idref=\"chapter\"", with: "idref=\"unknown\""),
                         package.replacingOccurrences(of: "href=\"chapter.xhtml\"", with: "href=\"../../escape\""),
                         package.replacingOccurrences(of: "href=\"chapter.xhtml\"", with: "href=\"%2e%2e/%2e%2e/escape\""),
                         package.replacingOccurrences(of: "id=\"chapter\"", with: "id=\"chapter\" fallback=\"chapter\""),
                         package.replacingOccurrences(of: "id=\"chapter\"", with: "id=\"chapter\" fallback=\"missing\"")] {
            let url = try epub(package: modified)
            defer { try? FileManager.default.removeItem(at: url) }
            XCTAssertThrowsError(try EpubInspector.inspect(url))
        }
    }
    func testRelativeEncodedResourceAndFragment() throws {
        let url = try epub(package: package.replacingOccurrences(of: "href=\"chapter.xhtml\"",
                                                               with: "href=\"../OEBPS/%63hapter.xhtml#start\""))
        defer { try? FileManager.default.removeItem(at: url) }
        XCTAssertEqual(try EpubInspector.inspect(url).title, "Café & Books")
    }

    func testCanonicalPackageAndPercentEncodedResourceNames() throws {
        let rawPackage = "OEBPS/cafe\u{301}.opf"
        let rawChapter = "OEBPS/re\u{301}sume\u{301}.xhtml"
        let xmlContainer = container.replacingOccurrences(of: "OEBPS/package.opf", with: rawPackage)
        let xmlPackage = package.replacingOccurrences(of: "chapter.xhtml", with: "r%C3%A9sum%C3%A9.xhtml#start")
        let url = try epub(container: xmlContainer, package: xmlPackage, packagePath: rawPackage, chapterPath: rawChapter)
        defer { try? FileManager.default.removeItem(at: url) }
        let metadata = try EpubInspector.inspect(url)
        XCTAssertEqual(metadata.packagePath, "OEBPS/café.opf")
        XCTAssertEqual(metadata.title, "Café & Books")
    }
    func testCanonicallyEquivalentManifestResourcesAreDuplicates() throws {
        let rawChapter = "OEBPS/cafe\u{301}.xhtml"
        let item = "<item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>"
        let replacement = "<item id=\"chapter\" href=\"caf%C3%A9.xhtml\" media-type=\"application/xhtml+xml\"/>" +
            "<item id=\"second\" href=\"cafe%CC%81.xhtml\" media-type=\"application/xhtml+xml\"/>"
        let url = try epub(package: package.replacingOccurrences(of: item, with: replacement), chapterPath: rawChapter)
        defer { try? FileManager.default.removeItem(at: url) }
        XCTAssertThrowsError(try EpubInspector.inspect(url)) { error in
            XCTAssertEqual(error as? ImportError, .integrity)
        }
    }
    func testImportPublishesValidatedContentAndDeduplicatesRenamedFiles() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let vaultURL = root.appendingPathComponent("vault")
        let databaseURL = root.appendingPathComponent("library.sqlite")
        let vault = try ContentVault(root: vaultURL)
        let library = try LibraryStore(url: databaseURL)
        let importer = ContentImporter(vault: vault, library: library)
        let source = try epub()
        defer { try? FileManager.default.removeItem(at: source) }
        let first = try await importer.importEPUB(source)
        let renamed = root.appendingPathComponent("renamed.epub")
        try FileManager.default.copyItem(at: source, to: renamed)
        let second = try await importer.importEPUB(renamed)
        XCTAssertEqual(first.content.id, second.content.id)
        let reopened = try LibraryStore(url: databaseURL)
        let stored = try await reopened.content(first.content.id)
        XCTAssertEqual(stored?.title, "Café & Books")
        XCTAssertEqual(stored?.authors, ["Ana", "José"])
        XCTAssertEqual(stored?.identifiers, ["urn:uuid:book"])
        XCTAssertEqual(stored?.languages, ["es"])
        XCTAssertEqual(stored?.originalFilename, "renamed.epub")
        let reopenedVault = try ContentVault(root: vaultURL)
        let object = try await reopenedVault.verifiedObject(first.content.id)
        XCTAssertEqual(object.length, first.content.length)
        XCTAssertEqual(try Data(contentsOf: object.url), try Data(contentsOf: source))
    }
    func testRejectedImportPublishesNoObjectOrStage() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let vaultURL = root.appendingPathComponent("vault")
        let vault = try ContentVault(root: vaultURL)
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let importer = ContentImporter(vault: vault, library: library)
        let source = try epub(mimetype: "application/zip")
        defer { try? FileManager.default.removeItem(at: source) }
        do { _ = try await importer.importEPUB(source); XCTFail("Invalid EPUB imported") }
        catch { XCTAssertEqual(error as? ImportError, .integrity) }
        for name in ["objects", "staging"] {
            XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: vaultURL.appendingPathComponent(name).path).isEmpty)
        }
    }
}

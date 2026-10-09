import Foundation
import XCTest
import ZIPFoundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

final class ReaderImportPublicationTests: XCTestCase, @unchecked Sendable {
    private struct Fixture {
        let root: URL
        let library: LibraryStore
        let storage: ReaderImportStorage
        let vault: ContentVault
        let importer: ContentImporter
        let job: ReaderImportJob
    }
    private func courseBytes() throws -> Data {
        var repository = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { repository.deleteLastPathComponent() }
        return try Data(contentsOf: repository.appendingPathComponent("test/tinta/fixtures/mini.pack"))
    }
    private func stage(_ bytes: Data, kind: ContentKind, actual: Data? = nil, formatVersion: UInt32 = 1, filename: String? = nil) async throws -> Fixture {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let storage = try ReaderImportStorage(root: root.appendingPathComponent("reader-imports"))
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let hash = try ContentID(SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined())
        let manifest = try ContentManifest(content: hash, kind: kind, length: UInt64(bytes.count), formatVersion: formatVersion,
                                           logicalIdentity: kind == .course ? Data(repeating: 7, count: 16) : Data(count: 16))
        let inventory = try ReaderInventory(reader: Data(repeating: 1, count: 16), generation: Data(repeating: 2, count: 16),
                                             contents: [manifest], complete: true)
        var job = try await library.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: Data(repeating: 3, count: 16))
        if let filename {
            let request = try ReaderContentMetadataRequest(generation: inventory.generation, manifest: manifest)
            let name = Data(filename.utf8)
            var bytes = Data([0x4c, 0x43, 0x4e, 1, 0])
            bytes.append(request.generation); bytes.append(manifest.encoded)
            bytes.append(UInt8(name.count)); bytes.append(name)
            let reply = try ReaderContentMetadataReply(decoding: bytes, request: request)
            _ = try await library.bindReaderImportFilename(job.id, request: request, reply: reply)
        }
        try await library.checkpointReaderImport(job.id, offset: 0, phase: .downloading)
        let download = actual ?? bytes
        for start in stride(from: 0, to: download.count, by: ReaderContentReadRequest.maximumChunkBytes) {
            let saved = try await library.readerImportJob(job.id); job = try XCTUnwrap(saved)
            let end = min(download.count, start + ReaderContentReadRequest.maximumChunkBytes)
            let offset = try await storage.append(download.subdata(in: start ..< end), to: job)
            try await library.checkpointReaderImport(job.id, offset: offset, phase: .downloading)
        }
        try await library.checkpointReaderImport(job.id, offset: UInt64(bytes.count), phase: .verifying)
        let saved = try await library.readerImportJob(job.id); job = try XCTUnwrap(saved)
        return Fixture(root: root, library: library, storage: storage, vault: vault,
                       importer: ContentImporter(vault: vault, library: library), job: job)
    }
    private func rejected(_ operation: () async throws -> Void) async {
        do { try await operation(); XCTFail("Invalid publication accepted") } catch {}
    }
    func testCoursePublicationCommitsAssociationSelectionAndCompletionAndRepeatDoesNotReselect() async throws {
        let fixture = try await stage(courseBytes(), kind: .course)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let content = try await fixture.importer.finishReaderImport(fixture.job.id, storage: fixture.storage, originalFilename: "Spanish.pack")
        XCTAssertEqual(content.id, fixture.job.manifest.content); XCTAssertEqual(content.kind, .course)
        let details = try await fixture.library.coursePackDetails(content.id); XCTAssertNotNil(details)
        let identity = try await fixture.library.courseIdentity(content.id); XCTAssertEqual(identity, fixture.job.manifest.logicalIdentity)
        let selected = try await fixture.library.isReaderContentSelected(reader: fixture.job.reader, content: content.id); XCTAssertTrue(selected)
        let saved = try await fixture.library.readerImportJob(fixture.job.id); XCTAssertEqual(saved?.phase, .completed)
        let restarted = try LibraryStore(url: fixture.root.appendingPathComponent("library.sqlite"))
        let pending = try await restarted.pendingReaderImports(); XCTAssertTrue(pending.isEmpty)
        _ = try await restarted.setReaderSelection(reader: fixture.job.reader, content: content.id, selected: false)
        let duplicate = try await fixture.importer.finishReaderImport(fixture.job.id, storage: fixture.storage, originalFilename: "ignored.pack")
        XCTAssertEqual(duplicate, content)
        let stillSelected = try await restarted.isReaderContentSelected(reader: fixture.job.reader, content: content.id); XCTAssertFalse(stillSelected)
        let terminal = try await restarted.readerImportJob(fixture.job.id)
        try await fixture.storage.discard(try XCTUnwrap(terminal))
        let recovered = try await fixture.importer.finishReaderImport(fixture.job.id, storage: fixture.storage, originalFilename: "Spanish.pack")
        XCTAssertEqual(recovered, content)
    }
    func testCorruptBytesAndWrongKindNeverPublishMetadataOrSelection() async throws {
        let bytes = try courseBytes()
        var corrupt = bytes; corrupt[corrupt.count - 1] ^= 1
        for fixture in [try await stage(bytes, kind: .course, actual: corrupt), try await stage(bytes, kind: .epub)] {
            defer { try? FileManager.default.removeItem(at: fixture.root) }
            await rejected { _ = try await fixture.importer.finishReaderImport(fixture.job.id, storage: fixture.storage,
                originalFilename: fixture.job.manifest.kind == .course ? "Spanish.pack" : "book.epub") }
            let ids = try await fixture.library.libraryContentIDs(); XCTAssertTrue(ids.isEmpty)
            let selections = try await fixture.library.readerSelections(reader: fixture.job.reader); XCTAssertTrue(selections.isEmpty)
            let job = try await fixture.library.readerImportJob(fixture.job.id); XCTAssertEqual(job?.phase, .verifying)
        }
    }
    func testInvalidFilenameDoesNotPublishAndRetryWithCorrectFilenameSucceeds() async throws {
        let fixture = try await stage(courseBytes(), kind: .course)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        for name in ["../Spanish.pack", "Spanish.epub", "", "folder/Spanish.pack"] {
            await rejected { _ = try await fixture.importer.finishReaderImport(fixture.job.id, storage: fixture.storage, originalFilename: name) }
        }
        let ids = try await fixture.library.libraryContentIDs(); XCTAssertTrue(ids.isEmpty)
        _ = try await fixture.importer.finishReaderImport(fixture.job.id, storage: fixture.storage, originalFilename: "Spanish.pack")
    }
    func testExistingCourseIdentityConflictRollsBackMetadataSelectionAndCompletion() async throws {
        let fixture = try await stage(courseBytes(), kind: .course)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let source = try await fixture.storage.completedSource(fixture.job)
        let metadata = try CoursePackInspector.inspect(source)
        let existing = LibraryContent(id: fixture.job.manifest.content, kind: .course, length: fixture.job.manifest.length,
                                      title: "Original", originalFilename: "original.pack", languages: [metadata.locale])
        try await fixture.library.putCoursePack(existing, metadata: metadata)
        _ = try await fixture.library.associateCourse(existing.id, confirmedIdentity: Data(repeating: 8, count: 16))
        await rejected { _ = try await fixture.importer.finishReaderImport(fixture.job.id, storage: fixture.storage, originalFilename: "Spanish.pack") }
        let stored = try await fixture.library.content(existing.id); XCTAssertEqual(stored, existing)
        let selections = try await fixture.library.readerSelections(reader: fixture.job.reader); XCTAssertTrue(selections.isEmpty)
        let job = try await fixture.library.readerImportJob(fixture.job.id); XCTAssertEqual(job?.phase, .verifying)
    }
    func testDeletionAfterObjectVerificationPreventsStalePublication() async throws {
        let fixture = try await stage(courseBytes(), kind: .course)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let source = try await fixture.storage.completedSource(fixture.job)
        let (_, metadata) = try await fixture.vault.importValidatedFile(source, expectedID: fixture.job.manifest.content,
            expectedLength: fixture.job.manifest.length) { try CoursePackInspector.inspect($0) }
        let deletion = try LibraryVisibilityChange(origin: Data(repeating: 9, count: 16), content: fixture.job.manifest.content,
                                                   removed: true, ancestors: [])
        _ = try await fixture.library.importLibraryVisibilityChanges([deletion])
        let content = LibraryContent(id: fixture.job.manifest.content, kind: .course, length: fixture.job.manifest.length,
                                      title: "Spanish", originalFilename: "Spanish.pack", languages: [metadata.locale])
        await rejected { try await fixture.library.publishReaderImport(fixture.job, content: content, courseMetadata: metadata) }
        let ids = try await fixture.library.libraryContentIDs(); XCTAssertTrue(ids.isEmpty)
        let details = try await fixture.library.coursePackDetails(content.id); XCTAssertNil(details)
        let selections = try await fixture.library.readerSelections(reader: fixture.job.reader); XCTAssertTrue(selections.isEmpty)
        let job = try await fixture.library.readerImportJob(fixture.job.id); XCTAssertEqual(job?.phase, .aborted)
    }
    func testEPUBPublicationUsesValidatedMetadataAndSelectsOnlySourceReader() async throws {
        let source = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".epub")
        defer { try? FileManager.default.removeItem(at: source) }
        let archive = try Archive(url: source, accessMode: .create)
        let entries = [
            ("mimetype", "application/epub+zip"),
            ("META-INF/container.xml", "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"book.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>"),
            ("book.opf", "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Reader Book</dc:title><dc:creator>Ana</dc:creator><dc:identifier>urn:reader:book</dc:identifier><dc:language>es</dc:language></metadata><manifest><item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/></manifest><spine><itemref idref=\"chapter\"/></spine></package>"),
            ("chapter.xhtml", "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>Reading</body></html>")
        ]
        for (path, text) in entries {
            let bytes = Data(text.utf8)
            try archive.addEntry(with: path, type: .file, uncompressedSize: Int64(bytes.count)) { offset, size in
                bytes.subdata(in: Int(offset) ..< min(bytes.count, Int(offset) + size))
            }
        }
        let fixture = try await stage(Data(contentsOf: source), kind: .epub)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let content = try await fixture.importer.finishReaderImport(fixture.job.id, storage: fixture.storage, originalFilename: "reader.epub")
        XCTAssertEqual(content.title, "Reader Book"); XCTAssertEqual(content.authors, ["Ana"])
        XCTAssertEqual(content.identifiers, ["urn:reader:book"]); XCTAssertEqual(content.languages, ["es"])
        let selected = try await fixture.library.isReaderContentSelected(reader: fixture.job.reader, content: content.id); XCTAssertTrue(selected)
        let other = try await fixture.library.readerSelections(reader: Data(repeating: 6, count: 16)); XCTAssertTrue(other.isEmpty)
        let stored = try await fixture.vault.verifiedObject(content.id); XCTAssertEqual(stored.length, content.length)
        try Data("corrupt".utf8).write(to: stored.url)
        await rejected { _ = try await fixture.importer.finishReaderImport(fixture.job.id, storage: fixture.storage, originalFilename: "reader.epub") }
        let completed = try await fixture.library.readerImportJob(fixture.job.id); XCTAssertEqual(completed?.phase, .completed)
    }

    private func fixtureBytes(_ name: String) throws -> Data {
        var repository = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { repository.deleteLastPathComponent() }
        return try Data(contentsOf: repository.appendingPathComponent("protocol/fixtures/" + name))
    }
    func testBitmapVectorAndCollectionFontPublicationPreservesFilenameAndSelection() async throws {
        for (name, filename, format): (String, String, UInt32) in [
            ("BitmapFont-v4.fixture", "Sample_18.cpfont", 4),
            ("VectorFont-sfnt.fixture", "Sample.ttf", 1),
            ("VectorFont-otto.fixture", "Sample.otf", 1),
            ("VectorFont-collection.fixture", "Collection.ttc", 1)
        ] {
            let bytes = try fixtureBytes(name)
            let f = try await stage(bytes, kind: .font, formatVersion: format)
            defer { try? FileManager.default.removeItem(at: f.root) }
            let content = try await f.importer.finishReaderImport(f.job.id, storage: f.storage, originalFilename: filename)
            XCTAssertEqual(content.kind, .font); XCTAssertEqual(content.originalFilename, filename)
            let selected = try await f.library.isReaderContentSelected(reader: f.job.reader, content: content.id); XCTAssertTrue(selected)
            let saved = try await f.library.readerImportJob(f.job.id); XCTAssertEqual(saved?.phase, .completed)
            let object = try await f.vault.verifiedObject(content.id); XCTAssertEqual(try Data(contentsOf: object.url), bytes)
            _ = try CloudContentDescriptor(content: content)
        }
    }
    func testPlainAndDictzipPublicationPreservesOriginalArchiveAndInspectedTitle() async throws {
        for name in ["DictionaryBundle-plain.fixture", "DictionaryBundle-dictzip.fixture"] {
            let bytes = try fixtureBytes(name)
            let f = try await stage(bytes, kind: .dictionary)
            defer { try? FileManager.default.removeItem(at: f.root) }
            let content = try await f.importer.finishReaderImport(f.job.id, storage: f.storage, originalFilename: "dictionary.zip")
            XCTAssertEqual(content.kind, .dictionary); XCTAssertEqual(content.title, "Canonical dictionary")
            let selected = try await f.library.isReaderContentSelected(reader: f.job.reader, content: content.id); XCTAssertTrue(selected)
            let saved = try await f.library.readerImportJob(f.job.id); XCTAssertEqual(saved?.phase, .completed)
            let object = try await f.vault.verifiedObject(content.id); XCTAssertEqual(try Data(contentsOf: object.url), bytes)
            let stages = try FileManager.default.contentsOfDirectory(atPath: f.root.appendingPathComponent("vault/staging").path)
            XCTAssertTrue(stages.isEmpty)
        }
    }

    func testBoundReaderFilenameControlsPublicationAndCompletedRetries() async throws {
        let fixture = try await stage(courseBytes(), kind: .course, filename: "Español.pack")
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        await rejected {
            _ = try await fixture.importer.finishReaderImport(fixture.job.id, storage: fixture.storage,
                                                              originalFilename: "changed.pack")
        }
        let before = try await fixture.library.content(fixture.job.manifest.content)
        XCTAssertNil(before)
        let content = try await fixture.importer.finishReaderImport(fixture.job.id, storage: fixture.storage)
        XCTAssertEqual(content.originalFilename, "Español.pack")
        let reopened = try LibraryStore(url: fixture.root.appendingPathComponent("library.sqlite"))
        let importer = ContentImporter(vault: fixture.vault, library: reopened)
        let repeated = try await importer.finishReaderImport(fixture.job.id, storage: fixture.storage)
        XCTAssertEqual(repeated, content)
        await rejected {
            _ = try await importer.finishReaderImport(fixture.job.id, storage: fixture.storage, originalFilename: "changed.pack")
        }
    }

    func testDeselectionFromAnotherStoreAbortsImportWithoutRemovingLibraryOrOtherReaderChoice() async throws {
        let f = try await stage(courseBytes(), kind: .course)
        defer { try? FileManager.default.removeItem(at: f.root) }
        let source = try await f.storage.completedSource(f.job)
        let metadata = try CoursePackInspector.inspect(source)
        let content = LibraryContent(id: f.job.manifest.content, kind: .course, length: f.job.manifest.length,
            title: "Existing", originalFilename: "existing.pack", languages: [metadata.locale])
        try await f.library.putCoursePack(content, metadata: metadata)
        let other = Data(repeating: 9, count: 16)
        _ = try await f.library.setReaderSelection(reader: other, content: content.id, selected: true)
        let reopened = try LibraryStore(url: f.root.appendingPathComponent("library.sqlite"))
        _ = try await reopened.setReaderSelection(reader: f.job.reader, content: content.id, selected: false)
        await rejected { _ = try await f.importer.finishReaderImport(f.job.id, storage: f.storage, originalFilename: "Spanish.pack") }
        let saved = try await f.library.readerImportJob(f.job.id)
        XCTAssertEqual(saved?.phase, .aborted); XCTAssertEqual(saved?.acknowledgedOffset, f.job.acknowledgedOffset)
        let selected = try await f.library.isReaderContentSelected(reader: f.job.reader, content: content.id)
        XCTAssertFalse(selected)
        let otherSelected = try await f.library.isReaderContentSelected(reader: other, content: content.id)
        XCTAssertTrue(otherSelected)
        let retained = try await f.library.content(content.id); XCTAssertEqual(retained, content)
    }

}

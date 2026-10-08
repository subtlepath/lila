import Foundation
import XCTest
import ZIPFoundation
@testable import CompanionKit

final class DictionaryBundleTests: XCTestCase, @unchecked Sendable {
    private func compressedFixture() throws -> Data {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { root.deleteLastPathComponent() }
        return try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/dictzip-chunks.dict.dz"))
    }
    private func compressedEntries(_ definitions: Data) -> [(String, Data)] {
        let index = record("whole", [0, 120_000])
        let info = "StarDict's dict ifo file\nversion=3.0.0\nbookname=Compressed dictionary\nwordcount=1\nidxfilesize=\(index.count)\n"
        return [("demo.ifo", Data(info.utf8)), ("demo.idx", index), ("demo.dict.dz", definitions)]
    }
    private func record(_ word: String, _ values: [UInt32]) -> Data {
        var data = Data(word.utf8); data.append(0)
        for value in values {
            for shift in [24, 16, 8, 0] { data.append(UInt8(truncatingIfNeeded: value >> shift)) }
        }
        return data
    }
    private func entries() -> [(String, Data)] {
        let index = record("apple", [0, 3]) + record("Banana", [3, 3])
        let info = "StarDict's dict ifo file\nversion=3.0.0\nbookname=Test dictionary\nwordcount=2\nidxfilesize=\(index.count)\nsynwordcount=1\n"
        return [("Dictionary/demo.ifo", Data(info.utf8)), ("Dictionary/demo.idx", index),
                ("Dictionary/demo.dict", Data("onetwo".utf8)), ("Dictionary/demo.syn", record("alias", [1]))]
    }
    private func zip(_ entries: [(String, Data)]) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".zip")
        let archive = try Archive(url: url, accessMode: .create)
        for (path, bytes) in entries {
            try archive.addEntry(with: path, type: .file, uncompressedSize: Int64(bytes.count), compressionMethod: .deflate) { offset, count in
                bytes.subdata(in: Int(offset) ..< min(bytes.count, Int(offset) + count))
            }
        }
        return url
    }
    func testValidatedBundleImportSurvivesRestart() async throws {
        let source = try zip(entries())
        defer { try? FileManager.default.removeItem(at: source) }
        let metadata = try DictionaryBundleInspector.inspect(source)
        XCTAssertEqual(metadata.basePath, "Dictionary/demo")
        XCTAssertEqual(metadata.members.count, 4)
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let database = root.appendingPathComponent("library.sqlite")
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let importer = ContentImporter(vault: vault, library: try LibraryStore(url: database))
        let imported = try await importer.importDictionaryBundle(source)
        let reopened = try LibraryStore(url: database)
        let content = try await reopened.content(imported.content.id)
        XCTAssertEqual(content?.kind, .dictionary)
        XCTAssertEqual(content?.title, "Test dictionary")
        let stored = try await vault.verifiedObject(imported.content.id)
        XCTAssertEqual(try Data(contentsOf: stored.url), try Data(contentsOf: source))
    }
    func testRejectsMissingMembersMultipleHeadersAndInvalidRanges() throws {
        var outOfBounds = entries(); outOfBounds[1].1 = record("apple", [UInt32.max, 3]) + record("Banana", [3, 3])
        var synonym = entries(); synonym[3].1 = record("alias", [2])
        for members in [Array(entries().dropLast()), entries().filter { !$0.0.hasSuffix(".dict") },
                        entries() + [("other.ifo", entries()[0].1)], outOfBounds, synonym] {
            let url = try zip(members)
            defer { try? FileManager.default.removeItem(at: url) }
            XCTAssertThrowsError(try DictionaryBundleInspector.inspect(url))
        }
    }
    func testCompressedImportAndTemporaryMemberCleanup() async throws {
        let source = try zip(compressedEntries(compressedFixture()))
        defer { try? FileManager.default.removeItem(at: source) }
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let vaultURL = root.appendingPathComponent("vault")
        let importer = ContentImporter(vault: try ContentVault(root: vaultURL),
                                       library: try LibraryStore(url: root.appendingPathComponent("library.sqlite")))
        let result = try await importer.importDictionaryBundle(source)
        XCTAssertEqual(result.content.title, "Compressed dictionary")
        XCTAssertEqual(result.metadata.members, ["demo.ifo", "demo.idx", "demo.dict.dz"])
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: vaultURL.appendingPathComponent("staging").path).isEmpty)
    }
    func testInnerDictzipFailureLeavesNoObjectsOrStages() async throws {
        var definitions = try compressedFixture()
        definitions[definitions.count - 8] ^= 1
        let source = try zip(compressedEntries(definitions))
        defer { try? FileManager.default.removeItem(at: source) }
        _ = try ArchiveValidator.validate(source)
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let vaultURL = root.appendingPathComponent("vault")
        let importer = ContentImporter(vault: try ContentVault(root: vaultURL),
                                       library: try LibraryStore(url: root.appendingPathComponent("library.sqlite")))
        do { _ = try await importer.importDictionaryBundle(source); XCTFail("Corrupt dictionary imported") }
        catch { XCTAssertEqual(error as? ImportError, .integrity) }
        for directory in ["objects", "staging"] {
            XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: vaultURL.appendingPathComponent(directory).path).isEmpty)
        }
    }
    func testFirmwareCanonicalDictionaryArchives() throws {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { root.deleteLastPathComponent() }
        for (name, members) in [("DictionaryBundle-plain.fixture", 4),
                                ("DictionaryBundle-dictzip.fixture", 4),
                                ("DictionaryBundle-no-syn.fixture", 3)] {
            let metadata = try DictionaryBundleInspector.inspect(root.appendingPathComponent("protocol/fixtures/" + name))
            XCTAssertEqual(metadata.basePath, "dictionary")
            XCTAssertEqual(metadata.info.name, "Canonical dictionary")
            XCTAssertEqual(metadata.info.wordCount, 2)
            XCTAssertEqual(metadata.members.count, members)
        }
    }
    func testSharedFirmwareDictionaryMetadata() throws {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { root.deleteLastPathComponent() }
        let info = try DictionaryInfo(data: Data(contentsOf: root.appendingPathComponent("protocol/fixtures/DictionaryInfo.fixture")))
        XCTAssertEqual(info.name, "Diccionario 中文")
        XCTAssertEqual(info.wordCount, 2)
        XCTAssertEqual(info.indexBytes, 24)
        XCTAssertEqual(info.synonymCount, 1)
        XCTAssertTrue(info.htmlDefinitions)
    }
    func testDecomposedMemberNamesResolveThroughValidatedCanonicalEntries() throws {
        let decomposed = "cafe\u{301}"
        let members = entries().map { path, bytes in
            (path.replacingOccurrences(of: "demo", with: decomposed), bytes)
        }
        let url = try zip(members)
        defer { try? FileManager.default.removeItem(at: url) }
        let metadata = try DictionaryBundleInspector.inspect(url)
        XCTAssertEqual(metadata.basePath, "Dictionary/café")
        XCTAssertEqual(metadata.members.count, 4)
        XCTAssertEqual(metadata.info.wordCount, 2)
    }

}

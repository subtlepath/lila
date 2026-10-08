import Foundation
import XCTest
import ZIPFoundation
@testable import CompanionKit

final class ArchiveValidatorTests: XCTestCase {
    private func archive(_ entries: [(String, Entry.EntryType, Data)]) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".zip")
        let archive = try Archive(url: url, accessMode: .create)
        for (path, type, data) in entries {
            try archive.addEntry(with: path, type: type, uncompressedSize: Int64(data.count),
                                 compressionMethod: .deflate) { offset, size in
                data.subdata(in: Int(offset) ..< min(data.count, Int(offset) + size))
            }
        }
        return url
    }
    func testStreamsCompressedEntriesAndDirectories() throws {
        let bytes = Data(repeating: 42, count: 150_000)
        let url = try archive([("OEBPS/", .directory, Data()), ("OEBPS/chapter.xhtml", .file, bytes)])
        defer { try? FileManager.default.removeItem(at: url) }
        let result = try ArchiveValidator.validate(url)
        XCTAssertEqual(result.paths, ["OEBPS", "OEBPS/chapter.xhtml"])
        XCTAssertEqual(result.uncompressedBytes, UInt64(bytes.count))
    }
    func testRejectsTraversalAliasesAndSymlinks() throws {
        for path in ["../escape", "/root", "a//b", "a/./b", "a\\b", "C:drive", "a\u{0}b"] {
            XCTAssertThrowsError(try ArchiveValidator.canonicalPath(path, directory: false))
        }
        for entries: [(String, Entry.EntryType, Data)] in [
            [("a", .symlink, Data("target".utf8))],
            [("a", .file, Data()), ("a", .file, Data())],
            [("é", .file, Data()), ("e\u{301}", .file, Data())]
        ] {
            let url = try archive(entries)
            defer { try? FileManager.default.removeItem(at: url) }
            XCTAssertThrowsError(try ArchiveValidator.validate(url))
        }
    }
    func testBoundsEntryCountAndExpandedBytes() throws {
        let url = try archive([("a", .file, Data(repeating: 0, count: 50)), ("b", .file, Data(repeating: 0, count: 50))])
        defer { try? FileManager.default.removeItem(at: url) }
        for limits in [ArchiveLimits(entries: 1), ArchiveLimits(entryBytes: 49), ArchiveLimits(totalBytes: 99)] {
            XCTAssertThrowsError(try ArchiveValidator.validate(url, limits: limits)) { error in
                XCTAssertEqual(error as? ImportError, .resourceLimit)
            }
        }
    }
    func testRejectsCorruptPayloadChecksum() throws {
        let marker = Data("uniquely identifiable payload".utf8)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".zip")
        defer { try? FileManager.default.removeItem(at: url) }
        do {
            let archive = try Archive(url: url, accessMode: .create)
            try archive.addEntry(with: "a", type: .file, uncompressedSize: Int64(marker.count)) { offset, size in
                marker.subdata(in: Int(offset) ..< min(marker.count, Int(offset) + size))
            }
        }
        var bytes = try Data(contentsOf: url)
        let range = try XCTUnwrap(bytes.range(of: marker))
        bytes[range.lowerBound] ^= 1
        try bytes.write(to: url)
        XCTAssertThrowsError(try ArchiveValidator.validate(url)) { error in
            XCTAssertEqual(error as? ImportError, .integrity)
        }
    }
    func testRejectsSilentIteratorTruncation() throws {
        let url = try archive([("a", .file, Data()), ("b", .file, Data())])
        defer { try? FileManager.default.removeItem(at: url) }
        var bytes = try Data(contentsOf: url)
        let signature = Data([0x50, 0x4b, 0x01, 0x02])
        let first = try XCTUnwrap(bytes.range(of: signature))
        let second = try XCTUnwrap(bytes.range(of: signature, in: first.upperBound ..< bytes.endIndex))
        bytes[second.lowerBound] = 0
        try bytes.write(to: url)
        XCTAssertThrowsError(try ArchiveValidator.validate(url))
    }
    func testZIP64EndRecordAndCountMismatch() throws {
        let url = try archive([("a", .file, Data("hello".utf8))])
        defer { try? FileManager.default.removeItem(at: url) }
        var bytes = try Data(contentsOf: url)
        let end = bytes.count - 22
        let original = bytes.subdata(in: end ..< bytes.count)
        func value(_ offset: Int) -> UInt64 {
            (0 ..< 4).reduce(0) { $0 | (UInt64(original[offset + $1]) << ($1 * 8)) }
        }
        func append(_ number: UInt64, _ width: Int, to data: inout Data) {
            for index in 0 ..< width { data.append(UInt8(truncatingIfNeeded: number >> (8 * index))) }
        }
        var record = Data()
        for (number, width): (UInt64, Int) in [(0x06064b50, 4), (44, 8), (45, 2), (45, 2),
            (0, 4), (0, 4), (1, 8), (1, 8), (value(12), 8), (value(16), 8),
            (0x07064b50, 4), (0, 4), (UInt64(end), 8), (1, 4)] {
            append(number, width, to: &record)
        }
        var ending = original
        for index in 8 ..< 12 { ending[index] = 0xff }
        bytes.removeSubrange(end ..< bytes.count)
        bytes.append(record); bytes.append(ending)
        try bytes.write(to: url)
        XCTAssertEqual(try ArchiveValidator.validate(url).uncompressedBytes, 5)
        bytes[end + 32] = 2
        try bytes.write(to: url)
        XCTAssertThrowsError(try ArchiveValidator.validate(url))
    }
}

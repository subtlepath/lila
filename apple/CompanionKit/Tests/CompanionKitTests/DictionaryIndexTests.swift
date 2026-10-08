import Foundation
import XCTest
@testable import CompanionKit

final class DictionaryIndexTests: XCTestCase {
    private func record(_ word: String, offset: UInt32, size: UInt32? = nil) -> Data {
        var bytes = Data(word.utf8); bytes.append(0)
        for value in [offset] + (size.map { [$0] } ?? []) {
            for shift in [24, 16, 8, 0] { bytes.append(UInt8(truncatingIfNeeded: value >> shift)) }
        }
        return bytes
    }
    func testEveryFragmentBoundaryAndReaderOrdering() throws {
        let bytes = record("apple", offset: 0, size: 2) + record("Banana", offset: 2, size: 3)
        for split in 0 ... bytes.count {
            var validator = DictionaryIndexValidator(words: 2, bytes: UInt64(bytes.count), mode: .definitions(length: 5))
            try validator.consume(bytes.prefix(split)); try validator.consume(bytes.dropFirst(split)); try validator.finish()
        }
    }
    func testRejectsOutOfOrderTruncatedOutOfBoundsAndCountMismatch() throws {
        let valid = record("apple", offset: 0, size: 2)
        for bytes in [record("z", offset: 0, size: 1) + valid, record("a", offset: 4, size: 2),
                      record("a", offset: 0, size: 0), Data(valid.dropLast()), Data([0, 0, 0, 0])] {
            var validator = DictionaryIndexValidator(words: 1, bytes: UInt64(bytes.count), mode: .definitions(length: 5))
            XCTAssertThrowsError(try { try validator.consume(bytes); try validator.finish() }())
        }
        var empty = DictionaryIndexValidator(words: 1, bytes: 0, mode: .definitions(length: 5))
        try empty.consume(Data())
        XCTAssertThrowsError(try empty.finish())
    }
    func testSynonymOrdinalAndOversizedHeadword() throws {
        let bytes = record("alias", offset: 1)
        var valid = DictionaryIndexValidator(words: 1, bytes: UInt64(bytes.count), mode: .synonyms(words: 2))
        try valid.consume(bytes); try valid.finish()
        var invalid = DictionaryIndexValidator(words: 1, bytes: UInt64(bytes.count), mode: .synonyms(words: 1))
        XCTAssertThrowsError(try invalid.consume(bytes))
        let long = record(String(repeating: "a", count: 256), offset: 0, size: 1)
        var oversized = DictionaryIndexValidator(words: 1, bytes: UInt64(long.count), mode: .definitions(length: 1))
        XCTAssertThrowsError(try oversized.consume(long)) { error in XCTAssertEqual(error as? ImportError, .resourceLimit) }
    }
    func testSharedFirmwareIndexFixturesAtEveryChunkSize() throws {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { root.deleteLastPathComponent() }
        for synonyms in [false, true] {
            let name = synonyms ? "DictionaryIndex-synonyms.fixture" : "DictionaryIndex-definitions.fixture"
            let bytes = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/" + name))
            for chunk in 1 ... bytes.count {
                var validator = DictionaryIndexValidator(words: synonyms ? 1 : 2, bytes: UInt64(bytes.count),
                    mode: synonyms ? .synonyms(words: 2) : .definitions(length: 6))
                for offset in stride(from: 0, to: bytes.count, by: chunk) {
                    try validator.consume(Data(bytes[offset ..< min(bytes.count, offset + chunk)]))
                }
                try validator.finish()
            }
        }
    }
}

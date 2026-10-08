import Foundation
import XCTest
@testable import CompanionKit

final class DictionaryInfoTests: XCTestCase {
    private let info = "StarDict's dict ifo file\nversion=3.0.0\nbookname=Español\nwordcount=2\nidxfilesize=40\nidxoffsetbits=32\nsametypesequence=h\nsynwordcount=1\n"
    func testMetadataAndCRLFCompatibility() throws {
        for text in [info, info.replacingOccurrences(of: "\n", with: "\r\n")] {
            let metadata = try DictionaryInfo(data: Data(text.utf8))
            XCTAssertEqual(metadata.name, "Español")
            XCTAssertEqual(metadata.wordCount, 2)
            XCTAssertEqual(metadata.indexBytes, 40)
            XCTAssertEqual(metadata.synonymCount, 1)
            XCTAssertTrue(metadata.htmlDefinitions)
        }
        let mixed = try DictionaryInfo(data: Data(info.replacingOccurrences(of: "sametypesequence=h", with: "sametypesequence=hm").utf8))
        XCTAssertFalse(mixed.htmlDefinitions)
    }
    func testRejectsUnsupportedOffsetsVersionsAndUnreadableFacts() throws {
        for text in [info.replacingOccurrences(of: "idxoffsetbits=32", with: "idxoffsetbits=64"),
                     info.replacingOccurrences(of: "version=3.0.0", with: "version=4.0.0"),
                     info.replacingOccurrences(of: "idxoffsetbits=32", with: "description=" + String(repeating: "x", count: 2048) + "\nidxoffsetbits=32")] {
            XCTAssertThrowsError(try DictionaryInfo(data: Data(text.utf8))) { error in
                XCTAssertEqual(error as? ImportError, .unsupportedEntry)
            }
        }
    }
    func testRejectsDuplicateFieldsNumericOverflowAndMalformedText() throws {
        for text in [info + "wordcount=3\n", info.replacingOccurrences(of: "wordcount=2", with: "wordcount=4294967296"),
                     info.replacingOccurrences(of: "idxfilesize=40", with: "idxfilesize=-1"),
                     info.replacingOccurrences(of: "sametypesequence=h", with: "sametypesequence=h1"), info + "\u{0}"] {
            XCTAssertThrowsError(try DictionaryInfo(data: Data(text.utf8)))
        }
        XCTAssertThrowsError(try DictionaryInfo(data: Data([0xff])))
        XCTAssertThrowsError(try DictionaryInfo(data: Data(repeating: 0, count: 65_537)))
    }
    func testUTF8ExtensionKeysUseExactBytes() throws {
        let metadata = try DictionaryInfo(data: Data((info + "é=first\ne\u{301}=second\n中文=value\n").utf8))
        XCTAssertEqual(metadata.wordCount, 2)
        for suffix in ["é=first\né=duplicate\n", "e\u{301}=first\ne\u{301}=duplicate\n"] {
            XCTAssertThrowsError(try DictionaryInfo(data: Data((info + suffix).utf8)))
        }
        var invalid = Data(info.utf8)
        invalid.append(contentsOf: [0xff, 61, 120, 10])
        XCTAssertThrowsError(try DictionaryInfo(data: invalid))
    }

}

import Foundation
import XCTest
@testable import CompanionKit

final class ReaderContentMetadataTests: XCTestCase {
    private func request(kind: ContentKind = .epub, format: UInt32 = 1) throws -> ReaderContentMetadataRequest {
        let manifest = try ContentManifest(content: ContentID(String(repeating: "12", count: 32)), kind: kind,
                                           length: 5, formatVersion: format, logicalIdentity: Data(repeating: 0, count: 16))
        return try ReaderContentMetadataRequest(generation: Data(repeating: 2, count: 16), manifest: manifest)
    }
    private func reply(_ request: ReaderContentMetadataRequest, name: Data, result: UInt8 = 0) -> Data {
        var data = Data([0x4c, 0x43, 0x4e, 1, result])
        data.append(request.generation); data.append(request.manifest.encoded)
        data.append(UInt8(name.count)); data.append(name)
        return data
    }
    func testExactRequestAndUnicodeFilenameRoundTrip() throws {
        let request = try request()
        XCTAssertEqual(request.encoded.count, 83)
        XCTAssertEqual(try ReaderContentMetadataRequest(decoding: request.encoded), request)
        let bytes = reply(request, name: Data("読書.epub".utf8))
        XCTAssertEqual(try ReaderContentMetadataReply(decoding: bytes, request: request).originalFilename, "読書.epub")
        for count in 0..<request.encoded.count {
            XCTAssertThrowsError(try ReaderContentMetadataRequest(decoding: Data(request.encoded.prefix(count))))
        }
        for count in 0..<bytes.count {
            XCTAssertThrowsError(try ReaderContentMetadataReply(decoding: Data(bytes.prefix(count)), request: request))
        }
        for index in [0, 3, 5, 21, 53, 62, 84] {
            var changed = bytes; changed[index] ^= 1
            XCTAssertThrowsError(try ReaderContentMetadataReply(decoding: changed, request: request))
        }
    }
    func testRejectsPathsControlsInvalidUTF8AndWrongFormat() throws {
        let request = try request()
        for name in ["", "../book.epub", "folder/book.epub", "folder\\book.epub", "bad\n.epub", "bad\u{7f}.epub", "book.zip"] {
            XCTAssertThrowsError(try ReaderContentMetadataReply(decoding: reply(request, name: Data(name.utf8)), request: request))
        }
        XCTAssertThrowsError(try ReaderContentMetadataReply(decoding: reply(request, name: Data([255])), request: request))
        let bitmap = try self.request(kind: .font, format: 4)
        XCTAssertThrowsError(try ReaderContentMetadataReply(decoding: reply(bitmap, name: Data("font.ttf".utf8)), request: bitmap))
        XCTAssertEqual(try ReaderContentMetadataReply(decoding: reply(bitmap, name: Data("font.cpfont".utf8)), request: bitmap).originalFilename, "font.cpfont")
        let vector = try self.request(kind: .font)
        XCTAssertThrowsError(try ReaderContentMetadataReply(decoding: reply(vector, name: Data("font.cpfont".utf8)), request: vector))
    }
    func testErrorRepliesCarryNoMetadata() throws {
        let request = try request()
        for result in ReaderContentReadResult.allCases where result != .ok {
            XCTAssertNil(try ReaderContentMetadataReply(decoding: reply(request, name: Data(), result: result.rawValue), request: request).originalFilename)
            XCTAssertThrowsError(try ReaderContentMetadataReply(decoding: reply(request, name: Data("book.epub".utf8), result: result.rawValue), request: request))
        }
    }
    func testSharedReaderWireFixtures() throws {
        var root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        for _ in 0..<4 { root.deleteLastPathComponent() }
        func fixture(_ name: String) throws -> Data {
            let data = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/\(name).json"))
            let json = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
            let hex = Array(try XCTUnwrap(json["binaryHex"] as? String))
            var bytes = Data()
            for index in stride(from: 0, to: hex.count, by: 2) {
                bytes.append(try XCTUnwrap(UInt8(String(hex[index...index + 1]), radix: 16)))
            }
            return bytes
        }
        let request = try request()
        XCTAssertEqual(try fixture("ReaderContentMetadataRequest"), request.encoded)
        let reply = try ReaderContentMetadataReply(decoding: fixture("ReaderContentMetadataReply"), request: request)
        XCTAssertEqual(reply.result, .ok)
        XCTAssertEqual(reply.originalFilename, "読書.epub")
    }

}

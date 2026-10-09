import Foundation
import XCTest
@testable import CompanionKit

final class ReaderContentReadTests: XCTestCase {
    private func request(offset: UInt64 = 0, count: UInt16 = 3) throws -> ReaderContentReadRequest {
        let manifest = try ContentManifest(content: ContentID(String(repeating: "12", count: 32)), kind: .epub,
                                           length: 5, formatVersion: 1, logicalIdentity: Data(repeating: 0, count: 16))
        return try ReaderContentReadRequest(generation: Data(repeating: 2, count: 16), manifest: manifest,
                                            offset: offset, maximumBytes: count)
    }
    private func reply(_ request: ReaderContentReadRequest, result: UInt8 = 0, bytes: Data) -> Data {
        var data = Data([0x4c, 0x43, 0x53, 1, result])
        data.append(request.generation); data.append(request.manifest.content.digest)
        data.appendLittleEndian(request.offset, count: 8)
        data.appendLittleEndian(UInt64(bytes.count), count: 2); data.append(bytes)
        return data
    }
    func testRequestRoundTripAndEveryTruncatedPrefix() throws {
        let request = try request()
        XCTAssertEqual(request.encoded.count, 93)
        XCTAssertEqual(try ReaderContentReadRequest(decoding: request.encoded), request)
        for count in 0..<93 { XCTAssertThrowsError(try ReaderContentReadRequest(decoding: Data(request.encoded.prefix(count)))) }
        XCTAssertThrowsError(try ReaderContentReadRequest(decoding: request.encoded + Data([0])))
        XCTAssertThrowsError(try self.request(offset: 5))
        XCTAssertThrowsError(try self.request(offset: UInt64.max))
        XCTAssertThrowsError(try self.request(count: 0))
        XCTAssertThrowsError(try self.request(count: UInt16(ReaderContentReadRequest.maximumChunkBytes + 1)))
    }
    func testReplyRequiresExactRequestBindingAndChunkLength() throws {
        let request = try request()
        let encoded = reply(request, bytes: Data([1, 2, 3]))
        XCTAssertEqual(try ReaderContentReadReply(decoding: encoded, request: request).bytes, Data([1, 2, 3]))
        for count in 0..<encoded.count {
            XCTAssertThrowsError(try ReaderContentReadReply(decoding: Data(encoded.prefix(count)), request: request))
        }
        for index in [0, 3, 5, 21, 53, 61] {
            var changed = encoded; changed[index] ^= 1
            XCTAssertThrowsError(try ReaderContentReadReply(decoding: changed, request: request))
        }
        XCTAssertThrowsError(try ReaderContentReadReply(decoding: encoded + Data([0]), request: request))
        XCTAssertThrowsError(try ReaderContentReadReply(decoding: reply(request, bytes: Data([1, 2])), request: request))
        let last = try self.request(offset: 4)
        XCTAssertEqual(try ReaderContentReadReply(decoding: reply(last, bytes: Data([5])), request: last).bytes, Data([5]))
        XCTAssertThrowsError(try ReaderContentReadReply(decoding: reply(last, bytes: Data([5, 6])), request: last))
    }
    func testRejectsInvalidIdentityAndUnsupportedManifestContracts() throws {
        let original = try request()
        for generation in [Data(), Data(repeating: 0, count: 16), Data(repeating: 2, count: 17)] {
            XCTAssertThrowsError(try ReaderContentReadRequest(generation: generation, manifest: original.manifest,
                                                             offset: 0, maximumBytes: 1))
        }
        for (kind, format, family, valid): (ContentKind, UInt32, UInt8, Bool) in [
            (.epub, 1, 0, true), (.epub, 2, 0, false), (.epub, 1, 1, false),
            (.course, 1, 1, true), (.course, 1, 0, false), (.course, 2, 1, false),
            (.font, 1, 0, true), (.font, 4, 0, true), (.font, 2, 0, false),
            (.dictionary, 1, 0, true), (.dictionary, 1, 1, false), (.firmware, 1, 0, false)
        ] {
            let manifest = try ContentManifest(content: original.manifest.content, kind: kind, length: 5,
                                               formatVersion: format, logicalIdentity: Data(repeating: family, count: 16))
            if valid {
                XCTAssertNoThrow(try ReaderContentReadRequest(generation: original.generation, manifest: manifest,
                                                              offset: 0, maximumBytes: 1))
            } else {
                XCTAssertThrowsError(try ReaderContentReadRequest(generation: original.generation, manifest: manifest,
                                                                 offset: 0, maximumBytes: 1))
            }
        }
        for (hash, length): (String, UInt64) in [(String(repeating: "0", count: 64), 5), (original.manifest.content.hex, 0)] {
            let manifest = try ContentManifest(content: ContentID(hash), kind: .epub, length: length,
                                               formatVersion: 1, logicalIdentity: Data(repeating: 0, count: 16))
            XCTAssertThrowsError(try ReaderContentReadRequest(generation: original.generation, manifest: manifest,
                                                             offset: 0, maximumBytes: 1))
        }
    }
    func testMaximumChunkFitsOneControlPayload() throws {
        let original = try request()
        let maximum = UInt16(ReaderContentReadRequest.maximumChunkBytes)
        let manifest = try ContentManifest(content: original.manifest.content, kind: .epub, length: UInt64(maximum),
                                           formatVersion: 1, logicalIdentity: Data(repeating: 0, count: 16))
        let request = try ReaderContentReadRequest(generation: original.generation, manifest: manifest,
                                                   offset: 0, maximumBytes: maximum)
        let encoded = reply(request, bytes: Data(repeating: 0xa5, count: Int(maximum)))
        XCTAssertEqual(encoded.count, ControlFrame.maximumPayload)
        XCTAssertEqual(try ReaderContentReadReply(decoding: encoded, request: request).bytes.count, Int(maximum))
    }
    func testSharedReadFixtures() throws {
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
        let request = try self.request()
        XCTAssertEqual(try fixture("ReaderContentReadRequest"), request.encoded)
        XCTAssertEqual(try ReaderContentReadReply(decoding: fixture("ReaderContentReadReply"), request: request).bytes,
                       Data([1, 2, 3]))
    }
    func testFailuresCannotCarryContentAndUnknownResultsAreRejected() throws {
        let request = try request()
        for result in ReaderContentReadResult.allCases where result != .ok {
            XCTAssertEqual(try ReaderContentReadReply(decoding: reply(request, result: result.rawValue, bytes: Data()),
                                                     request: request).result, result)
            XCTAssertThrowsError(try ReaderContentReadReply(decoding: reply(request, result: result.rawValue,
                                                                           bytes: Data([1])), request: request))
        }
        XCTAssertThrowsError(try ReaderContentReadReply(decoding: reply(request, result: 255, bytes: Data()), request: request))
    }
}

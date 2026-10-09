import Foundation
import XCTest
@testable import CompanionKit

final class ReaderContentHandoffTests: XCTestCase {
    private func request(offset: UInt64 = 17, length: UInt64 = 2 * 1024 * 1024) throws -> ReaderContentHandoffRequest {
        let manifest = try ContentManifest(content: ContentID(String(repeating: "12", count: 32)), kind: .epub,
            length: length, formatVersion: 1, logicalIdentity: Data(count: 16))
        return try ReaderContentHandoffRequest(transaction: Data(repeating: 3, count: 16),
            generation: Data(repeating: 2, count: 16), manifest: manifest, offset: offset)
    }
    func testRequestRoundTripRejectsTruncationAndThresholdAndInvalidOffset() throws {
        let request = try request()
        XCTAssertEqual(request.encoded.count, 107)
        XCTAssertEqual(try ReaderContentHandoffRequest(decoding: request.encoded), request)
        for count in 0..<107 {
            XCTAssertThrowsError(try ReaderContentHandoffRequest(decoding: Data(request.encoded.prefix(count))))
        }
        XCTAssertThrowsError(try self.request(offset: 1024 * 1024))
        XCTAssertThrowsError(try self.request(offset: UInt64.max))
        XCTAssertThrowsError(try self.request(offset: 0, length: 1024 * 1024))
        XCTAssertNoThrow(try self.request(offset: 0, length: 1024 * 1024 + 1))
    }
    func testReplyBindsTransactionCardHashAndDurableOffset() throws {
        let request = try request()
        var reply = Data([0x4c, 0x43, 0x54, 1, 0])
        reply.append(request.transaction); reply.append(request.read.generation)
        reply.append(request.read.manifest.content.digest); reply.appendLittleEndian(request.read.offset, count: 8)
        XCTAssertEqual(reply.count, 77)
        XCTAssertEqual(try ReaderContentHandoffReply(decoding: reply, request: request).result, .ok)
        for index in [0, 3, 5, 21, 37, 69] {
            var changed = reply; changed[index] ^= 1
            XCTAssertThrowsError(try ReaderContentHandoffReply(decoding: changed, request: request))
        }
        for count in 0..<77 {
            XCTAssertThrowsError(try ReaderContentHandoffReply(decoding: Data(reply.prefix(count)), request: request))
        }
        reply[4] = 255
        XCTAssertThrowsError(try ReaderContentHandoffReply(decoding: reply, request: request))
    }
    func testSharedFirmwareWireFixtures() throws {
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
        XCTAssertEqual(try fixture("ReaderContentHandoffRequest"), request.encoded)
        XCTAssertEqual(try ReaderContentHandoffReply(decoding: fixture("ReaderContentHandoffReply"), request: request).result, .ok)
    }

}

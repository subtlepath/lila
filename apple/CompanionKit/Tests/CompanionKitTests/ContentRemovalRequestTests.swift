import Foundation
import XCTest
@testable import CompanionKit

final class ContentRemovalRequestTests: XCTestCase {
    private func fixture(_ name: String = "ContentRemovalRequest") throws -> Data {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf:
            root.appendingPathComponent("protocol/fixtures/\(name).json"))) as! [String: Any]
        let hex = json["binaryHex"] as! String
        var data = Data(); data.reserveCapacity(hex.count / 2)
        var index = hex.startIndex
        while index < hex.endIndex {
            let end = hex.index(index, offsetBy: 2)
            data.append(UInt8(hex[index..<end], radix: 16)!); index = end
        }
        return data
    }
    func testSharedFixtureExactFramingAndOwnership() throws {
        let bytes = try fixture()
        let request = try ContentRemovalRequest(decoding: bytes)
        XCTAssertEqual(request.encoded, bytes)
        XCTAssertEqual(request.transaction, Data(repeating: 1, count: 16))
        XCTAssertEqual(request.owner, Data(repeating: 2, count: 16))
        XCTAssertEqual(request.generation, Data(repeating: 3, count: 16))
        XCTAssertEqual(request.manifest.kind, .dictionary)
        XCTAssertEqual(request.manifest.length, 1234)
        for size in 0..<bytes.count { XCTAssertThrowsError(try ContentRemovalRequest(decoding: Data(bytes.prefix(size)))) }
        XCTAssertThrowsError(try ContentRemovalRequest(decoding: bytes + Data([0])))
        for start in [4, 20, 36, 54] {
            var invalid = bytes
            invalid.replaceSubrange(start..<(start + (start == 54 ? 32 : 16)), with: Data(count: start == 54 ? 32 : 16))
            XCTAssertThrowsError(try ContentRemovalRequest(decoding: invalid))
        }
    }
    func testSupportedKindsAndManifestContract() throws {
        let fixture = try ContentRemovalRequest(decoding: fixture())
        for kind in [ContentKind.epub, .course, .font, .dictionary, .firmware] {
            let manifest = try ContentManifest(content: fixture.manifest.content, kind: kind, length: 1234,
                formatVersion: kind == .epub ? 0 : 1,
                logicalIdentity: Data(repeating: kind == .course ? 9 : 0, count: 16))
            if kind == .firmware {
                XCTAssertThrowsError(try ContentRemovalRequest(transaction: fixture.transaction, owner: fixture.owner,
                    generation: fixture.generation, manifest: manifest))
            } else {
                let request = try ContentRemovalRequest(transaction: fixture.transaction, owner: fixture.owner,
                    generation: fixture.generation, manifest: manifest)
                XCTAssertEqual(try ContentRemovalRequest(decoding: request.encoded), request)
            }
            let invalid = try ContentManifest(content: manifest.content, kind: kind, length: manifest.length,
                formatVersion: 99, logicalIdentity: manifest.logicalIdentity)
            XCTAssertThrowsError(try ContentRemovalRequest(transaction: fixture.transaction, owner: fixture.owner,
                generation: fixture.generation, manifest: invalid))
        }
    }
    func testNativeEPUBInventoryFormatOneAndLegacyFormatZero() throws {
        var bytes = try fixture()
        bytes[86] = UInt8(ContentKind.epub.rawValue)
        for format: UInt8 in [0, 1] {
            bytes[95] = format
            let request = try ContentRemovalRequest(decoding: bytes)
            XCTAssertEqual(request.manifest.kind, .epub)
            XCTAssertEqual(request.manifest.formatVersion, UInt32(format))
            XCTAssertEqual(request.encoded, bytes)
        }
        bytes[95] = 2
        XCTAssertThrowsError(try ContentRemovalRequest(decoding: bytes))
    }

    func testSharedRemovalReplyAndAuthenticatedCommand() throws {
        let request = try ContentRemovalRequest(decoding: fixture())
        let bytes = try fixture("ContentRemovalReply")
        let reply = try ContentRemovalReply(decoding: bytes, request: request)
        XCTAssertEqual(reply.result, .ok)
        XCTAssertEqual(reply.transaction, request.transaction)
        XCTAssertEqual(Command.removeContent.rawValue, 15)
        let frame = try ControlFrame(command: .removeContent, requestID: 42, payload: request.encoded)
        XCTAssertEqual(try ControlFrame(decoding: frame.encoded(), authenticated: true), frame)
        XCTAssertThrowsError(try ControlFrame(decoding: frame.encoded(), authenticated: false)) {
            XCTAssertEqual($0 as? ProtocolError, .unauthorized)
        }
    }

    func testRemovalReplyRejectsTruncationUnknownStatusAndAnotherTransaction() throws {
        let request = try ContentRemovalRequest(decoding: fixture())
        let bytes = try fixture("ContentRemovalReply")
        for size in 0..<bytes.count {
            XCTAssertThrowsError(try ContentRemovalReply(decoding: bytes.prefix(size), request: request))
        }
        XCTAssertThrowsError(try ContentRemovalReply(decoding: bytes + Data([0]), request: request))
        var changed = bytes
        changed[0] = 10
        XCTAssertThrowsError(try ContentRemovalReply(decoding: changed, request: request))
        changed = bytes
        changed[1] ^= 1
        XCTAssertThrowsError(try ContentRemovalReply(decoding: changed, request: request))
    }

    func testEveryRemovalResultAndInstallationAuthorizationFailure() throws {
        let request = try ContentRemovalRequest(decoding: fixture())
        for result in ContentRemovalResult.allCases {
            let bytes = Data([result.rawValue]) + request.transaction
            XCTAssertEqual(try ContentRemovalReply(decoding: bytes, request: request).result, result)
        }
        let denied = Data([ContentRemovalResult.unauthorized.rawValue]) + Data(repeating: 0, count: 16)
        XCTAssertThrowsError(try ContentRemovalReply(decoding: denied, request: request)) {
            XCTAssertEqual($0 as? ProtocolError, .unauthorized)
        }
        let falseSuccess = Data([ContentRemovalResult.ok.rawValue]) + Data(repeating: 0, count: 16)
        XCTAssertThrowsError(try ContentRemovalReply(decoding: falseSuccess, request: request))
    }

    func testRemovalCapabilityIsIndependentAndExplicit() {
        XCTAssertFalse(ReaderCapabilities([.declaredTransfers, .courseTransfers, .dictionaryTransfers]).supportsEpubRemoval)
        XCTAssertTrue(ReaderCapabilities.epubRemovals.supportsEpubRemoval)
        XCTAssertEqual(ReaderCapabilities.epubRemovals.rawValue, 256)
    }

}

import Foundation
import XCTest
@testable import CompanionKit

final class WifiHandoffOfferTests: XCTestCase {
    func testSharedFixtureRoundTripBindingAndMalformedOffers() throws {
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { workspace.deleteLastPathComponent() }
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf:
            workspace.appendingPathComponent("protocol/fixtures/WifiHandoffOffer.json"))) as? [String: Any]
        let hex = try XCTUnwrap(json?["binaryHex"] as? String)
        var bytes = Data()
        bytes.reserveCapacity(hex.count / 2)
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            bytes.append(try XCTUnwrap(UInt8(hex[index..<next], radix: 16)))
            index = next
        }
        let offer = try WifiHandoffOffer(decoding: bytes)
        XCTAssertEqual(offer.encoded, bytes)
        XCTAssertEqual(offer.discoveryName, "lila-05050505050505050505050505050505")
        XCTAssertEqual(WifiHandoffOffer.discoveryServiceType, "_lila-sync._tcp.")
        var differentlyNamed = bytes; differentlyNamed[65] = 0xab
        XCTAssertEqual(try WifiHandoffOffer(decoding: differentlyNamed).discoveryName,
                       "lila-ab050505050505050505050505050505")
        XCTAssertEqual(offer.port, 8080)
        XCTAssertEqual(offer.lifetimeSeconds, 30)
        let expected = [offer.reader, offer.storageGeneration, offer.installation, offer.transaction]
        XCTAssertTrue(offer.matches(reader: expected[0], storageGeneration: expected[1], installation: expected[2], transaction: expected[3]))
        for at in 0..<4 {
            var mismatched = expected
            mismatched[at] = Data(repeating: 9, count: 16)
            XCTAssertFalse(offer.matches(reader: mismatched[0], storageGeneration: mismatched[1], installation: mismatched[2], transaction: mismatched[3]))
        }
        for offset in [1, 17, 33, 49, 65, 81] {
            var invalid = bytes
            invalid.replaceSubrange(offset..<(offset + (offset == 81 ? 32 : 16)), with: Data(repeating: 0, count: offset == 81 ? 32 : 16))
            XCTAssertThrowsError(try WifiHandoffOffer(decoding: invalid))
        }
        for count in 0..<bytes.count { XCTAssertThrowsError(try WifiHandoffOffer(decoding: bytes.prefix(count))) }
        for (offset, value): (Int, UInt8) in [(0, 2), (113, 0), (113, 127), (113, 224), (119, 0), (119, 121)] {
            var invalid = bytes; invalid[offset] = value
            XCTAssertThrowsError(try WifiHandoffOffer(decoding: invalid))
        }
        var discoveredBytes = bytes
        discoveredBytes.replaceSubrange(113..<117, with: Data(repeating: 0, count: 4))
        let discovered = try WifiHandoffOffer(decoding: discoveredBytes)
        XCTAssertTrue(discovered.requiresDiscovery)
        XCTAssertFalse(offer.requiresDiscovery)
        XCTAssertEqual(discovered.encoded, discoveredBytes)
        XCTAssertThrowsError(try WifiHTTPMessageTransport(offer: discovered))
        for invalid in [Data(), Data([0, 1, 2, 3]), Data([127, 0, 0, 1]), Data([224, 0, 0, 1])] {
            XCTAssertThrowsError(try WifiHTTPMessageTransport(offer: discovered, resolvedAddress: invalid))
        }
        XCTAssertThrowsError(try WifiHTTPMessageTransport(offer: offer, resolvedAddress: offer.address))
        _ = try WifiHTTPMessageTransport(offer: discovered, resolvedAddress: offer.address)
        var oversized = bytes; oversized.append(0)
        XCTAssertThrowsError(try WifiHandoffOffer(decoding: oversized))
    }
}

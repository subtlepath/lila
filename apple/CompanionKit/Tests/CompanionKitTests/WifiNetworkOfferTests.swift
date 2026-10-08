import Foundation
import XCTest
@testable import CompanionKit

final class WifiNetworkOfferTests: XCTestCase {
    private func fixture(_ name: String) throws -> Data {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf: root.appendingPathComponent("protocol/fixtures/\(name).json"))) as? [String: Any]
        let hex = try XCTUnwrap(json?["binaryHex"] as? String)
        var bytes = Data(); bytes.reserveCapacity(hex.count / 2)
        var at = hex.startIndex
        while at < hex.endIndex {
            let end = hex.index(at, offsetBy: 2)
            bytes.append(try XCTUnwrap(UInt8(hex[at..<end], radix: 16))); at = end
        }
        return bytes
    }
    func testSharedFixturesRoundTripAndRejectMalformedEnvelopes() throws {
        for hotspot in [false, true] {
            let bytes = try fixture(hotspot ? "WifiNetworkOfferHotspot" : "WifiNetworkOfferSaved")
            let value = try WifiNetworkOffer(decoding: bytes)
            XCTAssertEqual(value.encoded, bytes)
            XCTAssertEqual(value.mode, hotspot ? .hotspot : .savedNetwork)
            XCTAssertEqual(value.ssid, Data((hotspot ? "lila-test" : "Reader Home").utf8))
            XCTAssertEqual(value.password, Data((hotspot ? "fixture-password" : "").utf8))
            XCTAssertEqual(value.offer.requiresDiscovery, !hotspot)
            for length in 0..<bytes.count { XCTAssertThrowsError(try WifiNetworkOffer(decoding: bytes.prefix(length))) }
            for at in [0, 1, 122, 123, 124] {
                var invalid = bytes; invalid[at] = 255
                XCTAssertThrowsError(try WifiNetworkOffer(decoding: invalid))
            }
            var invalid = bytes; invalid[125] = 0
            XCTAssertThrowsError(try WifiNetworkOffer(decoding: invalid))
            invalid = bytes; invalid.append(0)
            XCTAssertThrowsError(try WifiNetworkOffer(decoding: invalid))
        }
    }
    func testBoundsSavedPasswordPrivacyAndRawSavedSSID() throws {
        let base = try WifiNetworkOffer(decoding: fixture("WifiNetworkOfferHotspot")).offer
        let maximum = try WifiNetworkOffer(offer: base, mode: .hotspot,
            ssid: Data(repeating: 115, count: 32), password: Data(repeating: 112, count: 63))
        XCTAssertEqual(maximum.encoded.count, WifiNetworkOffer.maximumSize)
        XCTAssertEqual(try WifiNetworkOffer(decoding: maximum.encoded), maximum)
        let raw = try WifiNetworkOffer(offer: base, mode: .savedNetwork, ssid: Data([110, 255]))
        XCTAssertEqual(try WifiNetworkOffer(decoding: raw.encoded), raw)
        for (mode, ssid, password): (WifiNetworkMode, Data, Data) in [
            (.savedNetwork, Data("ssid".utf8), Data("private-network-password".utf8)),
            (.savedNetwork, Data(), Data()), (.savedNetwork, Data([0]), Data()),
            (.hotspot, Data(repeating: 115, count: 33), Data(repeating: 112, count: 63)),
            (.hotspot, Data("ssid".utf8), Data(repeating: 112, count: 64)),
            (.hotspot, Data("ssid".utf8), Data("short".utf8)),
            (.hotspot, Data("line\nfeed".utf8), Data("valid-password".utf8)),
            (.hotspot, Data("ssid".utf8), Data("bad\npassword".utf8)),
            (.hotspot, Data([110, 255]), Data("valid-password".utf8))] {
            XCTAssertThrowsError(try WifiNetworkOffer(offer: base, mode: mode, ssid: ssid, password: password))
        }
        var leaked = maximum.encoded; leaked[122] = WifiNetworkMode.savedNetwork.rawValue
        XCTAssertThrowsError(try WifiNetworkOffer(decoding: leaked))
    }
}

import Foundation
import XCTest
@testable import CompanionKit

final class WifiHandoffCommandsTests: XCTestCase {
    private func json(_ name: String) throws -> [String: Any] {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        return try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: root.appendingPathComponent("protocol/fixtures/\(name).json"))) as? [String: Any])
    }
    private func bytes(_ hex: String) throws -> Data {
        var result = Data(); result.reserveCapacity(hex.count / 2)
        var at = hex.startIndex
        while at < hex.endIndex {
            let end = hex.index(at, offsetBy: 2); result.append(try XCTUnwrap(UInt8(hex[at..<end], radix: 16))); at = end
        }
        return result
    }
    func testSharedCommandFixturesAndInvalidIdentities() throws {
        let fixture = try XCTUnwrap(json("WifiHandoffCommands")["binaryHex"] as? [String: String])
        let transaction = Data(repeating: 4, count: 16), session = Data(repeating: 5, count: 16)
        let requests = [
            "prepareSaved": try WifiHandoffCommands.prepare(mode: .savedNetwork, transaction: transaction, requestID: 1),
            "prepareHotspot": try WifiHandoffCommands.prepare(mode: .hotspot, transaction: transaction, requestID: 2),
            "activate": try WifiHandoffCommands.activate(transaction: transaction, session: session, requestID: 3),
            "cancel": try WifiHandoffCommands.cancel(transaction: transaction, session: session, requestID: 4)]
        for (name, request) in requests {
            XCTAssertEqual(request.payload, try bytes(XCTUnwrap(fixture[name])))
            XCTAssertEqual(request.command, .wifiHandoff); XCTAssertFalse(request.response)
        }
        for invalid in [Data(), Data(repeating: 0, count: 16), Data(repeating: 1, count: 15), Data(repeating: 1, count: 17)] {
            XCTAssertThrowsError(try WifiHandoffCommands.prepare(mode: .savedNetwork, transaction: invalid, requestID: 1))
            XCTAssertThrowsError(try WifiHandoffCommands.activate(transaction: invalid, session: session, requestID: 1))
            XCTAssertThrowsError(try WifiHandoffCommands.cancel(transaction: transaction, session: invalid, requestID: 1))
        }
    }
    func testOfferRequiresModeReaderCardInstallationAndTransactionBindings() throws {
        let offered = try WifiNetworkOffer(decoding: bytes(XCTUnwrap(json("WifiNetworkOfferHotspot")["binaryHex"] as? String)))
        let request = try WifiHandoffCommands.prepare(mode: .hotspot, transaction: offered.offer.transaction, requestID: 7)
        let reply = try ControlFrame(command: .wifiHandoff, response: true, requestID: 7, payload: offered.encoded)
        let identities = [offered.offer.reader, offered.offer.storageGeneration, offered.offer.installation]
        XCTAssertEqual(try WifiHandoffCommands.offer(reply, to: request, reader: identities[0], storageGeneration: identities[1], installation: identities[2]), offered)
        for at in 0..<3 {
            var wrong = identities; wrong[at] = Data(repeating: 9, count: 16)
            XCTAssertThrowsError(try WifiHandoffCommands.offer(reply, to: request, reader: wrong[0], storageGeneration: wrong[1], installation: wrong[2]))
        }
        for badRequest in [
            try WifiHandoffCommands.prepare(mode: .savedNetwork, transaction: offered.offer.transaction, requestID: 7),
            try WifiHandoffCommands.prepare(mode: .hotspot, transaction: Data(repeating: 9, count: 16), requestID: 7)] {
            XCTAssertThrowsError(try WifiHandoffCommands.offer(reply, to: badRequest, reader: identities[0], storageGeneration: identities[1], installation: identities[2]))
        }
        for badReply in [try ControlFrame(command: .wifiHandoff, response: true, requestID: 8, payload: offered.encoded),
                         try ControlFrame(command: .wifiHandoff, requestID: 7, payload: offered.encoded),
                         try ControlFrame(command: .inventory, response: true, requestID: 7, payload: offered.encoded)] {
            XCTAssertThrowsError(try WifiHandoffCommands.offer(badReply, to: request, reader: identities[0], storageGeneration: identities[1], installation: identities[2]))
        }
    }
    func testActivationAndCancellationAcknowledgementsAndRemoteErrors() throws {
        for request in [try WifiHandoffCommands.activate(transaction: Data(repeating: 4, count: 16), session: Data(repeating: 5, count: 16), requestID: 7),
                        try WifiHandoffCommands.cancel(transaction: Data(repeating: 4, count: 16), session: Data(repeating: 5, count: 16), requestID: 7)] {
            let reply = try ControlFrame(command: .wifiHandoff, response: true, requestID: 7, payload: Data([0]))
            XCTAssertNoThrow(try WifiHandoffCommands.acknowledgement(reply, to: request))
            for payload in [Data(), Data([1]), Data([0, 0])] {
                XCTAssertThrowsError(try WifiHandoffCommands.acknowledgement(ControlFrame(command: .wifiHandoff, response: true, requestID: 7, payload: payload), to: request))
            }
            let rejected = try ControlFrame(command: .error, response: true, requestID: 7, payload: Data([2]))
            XCTAssertThrowsError(try WifiHandoffCommands.acknowledgement(rejected, to: request)) { error in
                XCTAssertEqual(error as? WifiHandoffCommandError, .control(2))
            }
            for at in [0, 1, 2, 18] {
                var invalid = request.payload
                if at < 2 { invalid[at] = 255 } else { invalid.replaceSubrange(at..<at + 16, with: Data(repeating: 0, count: 16)) }
                let invalidRequest = try ControlFrame(command: .wifiHandoff, requestID: 7, payload: invalid)
                XCTAssertThrowsError(try WifiHandoffCommands.acknowledgement(reply, to: invalidRequest))
            }
        }
    }
}

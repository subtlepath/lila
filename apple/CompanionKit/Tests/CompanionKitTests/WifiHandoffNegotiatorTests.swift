import Foundation
import XCTest
@testable import CompanionKit

private enum NegotiationFailure: Error { case disconnected, malformed }
private actor NegotiationWire: CompanionTransport {
    let network: WifiNetworkOffer
    let failure: NegotiationFailure?
    let reject: Bool
    var requests: [ControlFrame] = []
    init(_ network: WifiNetworkOffer, failure: NegotiationFailure? = nil, reject: Bool = false) {
        self.network = network; self.failure = failure; self.reject = reject
    }
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        requests.append(request)
        if request.payload[1] == 1 {
            return try ControlFrame(command: .wifiHandoff, response: true, requestID: request.requestID, payload: network.encoded)
        }
        if let failure { throw failure }
        return try ControlFrame(command: reject ? .error : .wifiHandoff, response: true,
                                requestID: request.requestID, payload: Data([reject ? 5 : 0]))
    }
}
final class WifiHandoffNegotiatorTests: XCTestCase {
    private func offer() throws -> WifiNetworkOffer {
        var bytes = Data([1])
        for value: UInt8 in [1, 2, 3, 4, 5] { bytes.append(Data(repeating: value, count: 16)) }
        bytes.append(Data(repeating: 6, count: 32)); bytes.append(Data([0, 0, 0, 0, 0x90, 0x1f, 30, 0]))
        return try WifiNetworkOffer(offer: WifiHandoffOffer(decoding: bytes), mode: .hotspot,
                                     ssid: Data("lila-test".utf8), password: Data("0123456789abcdef".utf8))
    }
    private func negotiate(_ actor: WifiHandoffNegotiator, _ wire: NegotiationWire, reader: Data? = nil) async throws -> WifiHandoffNegotiation {
        let network = await wire.network
        return try await actor.negotiate(transport: wire, reader: reader ?? network.offer.reader,
                                         storageGeneration: network.offer.storageGeneration,
                                         installation: network.offer.installation, transaction: network.offer.transaction,
                                         mode: .hotspot, uncertainActivation: { error in
                                             if case NegotiationFailure.disconnected = error { return true }
                                             return false
                                         }, clock: { 100 })
    }
    func testValidatedOfferActivatesWithDistinctRequestIDs() async throws {
        let network = try offer(), wire = NegotiationWire(network), actor = WifiHandoffNegotiator()
        let result = try await negotiate(actor, wire)
        XCTAssertEqual(result.network, network); XCTAssertEqual(result.receivedAtNanoseconds, 100)
        XCTAssertTrue(result.activationAcknowledged)
        let requests = await wire.requests
        XCTAssertEqual(requests.map(\.requestID), [1, 2])
        XCTAssertEqual(requests[1].payload, try WifiHandoffCommands.activate(transaction: network.offer.transaction,
            session: network.offer.session, requestID: 2).payload)
    }
    func testOnlyClassifiedTransportLossAllowsUncertainActivation() async throws {
        let wire = NegotiationWire(try offer(), failure: .disconnected)
        let result = try await negotiate(WifiHandoffNegotiator(), wire)
        XCTAssertFalse(result.activationAcknowledged)
        let malformed = NegotiationWire(try offer(), failure: .malformed)
        do { _ = try await negotiate(WifiHandoffNegotiator(), malformed); XCTFail() }
        catch NegotiationFailure.malformed {} catch { XCTFail("Unexpected error: \(error)") }
        let rejected = NegotiationWire(try offer(), reject: true)
        do { _ = try await negotiate(WifiHandoffNegotiator(), rejected); XCTFail() }
        catch WifiHandoffCommandError.control(5) {} catch { XCTFail("Unexpected error: \(error)") }
    }
    func testForeignOfferNeverActivatesAndFailureReleasesActor() async throws {
        let wire = NegotiationWire(try offer()), actor = WifiHandoffNegotiator()
        do { _ = try await negotiate(actor, wire, reader: Data(repeating: 9, count: 16)); XCTFail() }
        catch WifiHandoffCommandError.binding {} catch { XCTFail("Unexpected error: \(error)") }
        let requests = await wire.requests
        XCTAssertEqual(requests.count, 1)
        _ = try await negotiate(actor, wire)
        let retried = await wire.requests
        XCTAssertEqual(retried.map(\.requestID), [1, 3, 4])
    }
    func testExpiredOrBackwardClockNeverSendsActivation() async throws {
        final class Clock: @unchecked Sendable {
            let lock = NSLock()
            var reads = 0
            let backward: Bool
            init(_ backward: Bool) { self.backward = backward }
            func now() -> UInt64 { lock.withLock {
                reads += 1
                return reads == 1 ? 100 : backward ? 99 : 30_000_000_100
            } }
        }
        for backward in [false, true] {
            let network = try offer(), wire = NegotiationWire(network), clock = Clock(backward)
            do {
                _ = try await WifiHandoffNegotiator().negotiate(transport: wire, reader: network.offer.reader,
                    storageGeneration: network.offer.storageGeneration, installation: network.offer.installation,
                    transaction: network.offer.transaction, mode: .hotspot, uncertainActivation: { _ in true },
                    clock: { clock.now() })
                XCTFail()
            } catch WifiHandoffTransportError.expired {} catch { XCTFail("Unexpected error: \(error)") }
            let requests = await wire.requests
            XCTAssertEqual(requests.count, 1)
        }
    }
    func testCancellationBeforeNegotiationSendsNothing() async throws {
        let network = try offer(), wire = NegotiationWire(network)
        let task = Task {
            withUnsafeCurrentTask { $0?.cancel() }
            return try await WifiHandoffNegotiator().negotiate(transport: wire, reader: network.offer.reader,
                storageGeneration: network.offer.storageGeneration, installation: network.offer.installation,
                transaction: network.offer.transaction, mode: .hotspot, uncertainActivation: { _ in false })
        }
        do { _ = try await task.value; XCTFail() }
        catch is CancellationError {} catch { XCTFail("Unexpected error: \(error)") }
        let requests = await wire.requests
        XCTAssertTrue(requests.isEmpty)
    }

}

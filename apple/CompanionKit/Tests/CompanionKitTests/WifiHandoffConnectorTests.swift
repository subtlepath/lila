import Foundation
import XCTest
@testable import CompanionKit

private final class ConnectorClock: @unchecked Sendable {
    private let lock = NSLock()
    private var value: UInt64 = 0
    func set(_ value: UInt64) { lock.withLock { self.value = value } }
    func now() -> UInt64 { lock.withLock { value } }
}
private actor ConnectorWire: WifiMessageTransport {
    private let cipher: WifiMessageCipher
    private var timeout: UInt64 = 0
    private var closed = false
    init(_ offer: WifiHandoffOffer) throws {
        cipher = try WifiMessageCipher(key: offer.key, session: offer.session, sending: .readerToApple)
    }
    func exchange(_ message: Data, timeoutNanoseconds: UInt64) async throws -> Data {
        timeout = timeoutNanoseconds
        let request = try ControlFrame(decoding: await cipher.open(message), authenticated: true)
        let response = try ControlFrame(command: request.command, response: true, requestID: request.requestID)
        return try await cipher.seal(response.encoded())
    }
    func close() async { closed = true; await cipher.invalidate() }
    func state() -> (UInt64, Bool) { (timeout, closed) }
}
final class WifiHandoffConnectorTests: XCTestCase {
    private func offer(discovery: Bool = true) throws -> WifiHandoffOffer {
        var bytes = Data([1])
        for value: UInt8 in 1...5 { bytes.append(Data(repeating: value, count: 16)) }
        bytes.append(Data(0..<32)); bytes.append(contentsOf: discovery ? [0, 0, 0, 0] : [192, 168, 4, 1])
        bytes.appendLittleEndian(8080, count: 2); bytes.appendLittleEndian(30, count: 2)
        return try WifiHandoffOffer(decoding: bytes)
    }
    @MainActor
    func testBothEndpointModesComposeEncryptedExchangeAndKeepOriginalDeadline() async throws {
        for discovery in [false, true] {
            let offer = try offer(discovery: discovery)
            let clock = ConnectorClock(); clock.set(5_000_000_000)
            let wire = try ConnectorWire(offer)
            var resolutions = 0, creations = 0
            let transport = try await WifiHandoffConnector.prepare(offer: offer, reader: offer.reader,
                storageGeneration: offer.storageGeneration, installation: offer.installation,
                transaction: offer.transaction, receivedAtNanoseconds: 0,
                resolve: { resolvedOffer, receivedAt in
                    resolutions += 1
                    XCTAssertEqual(resolvedOffer, offer); XCTAssertEqual(receivedAt, 0)
                    clock.set(25_000_000_000)
                    return Data([192, 168, 4, 2])
                }, makeWire: { selectedOffer, address in
                    creations += 1; XCTAssertEqual(selectedOffer, offer)
                    XCTAssertEqual(address, discovery ? Data([192, 168, 4, 2]) : nil)
                    return wire
                }, clock: { clock.now() })
            XCTAssertEqual(resolutions, discovery ? 1 : 0); XCTAssertEqual(creations, 1)
            let request = try ControlFrame(command: .transferStatus, requestID: 7, payload: offer.transaction)
            let reply = try await transport.exchange(request)
            XCTAssertEqual(reply.command, .transferStatus); XCTAssertEqual(reply.requestID, 7)
            let state = await wire.state()
            XCTAssertEqual(state.0, discovery ? 5_000_000_000 : 25_000_000_000)
            XCTAssertFalse(state.1)
            await transport.close()
            let closed = await wire.state(); XCTAssertTrue(closed.1)
        }
    }
    @MainActor
    func testWrongBindingAndExpiredOffersHaveNoDiscoveryOrHTTPSetup() async throws {
        let offer = try offer()
        let expected = [offer.reader, offer.storageGeneration, offer.installation, offer.transaction]
        for changed in 0..<4 {
            var identities = expected; identities[changed] = Data(repeating: 9, count: 16)
            do {
                _ = try await WifiHandoffConnector.prepare(offer: offer, reader: identities[0],
                    storageGeneration: identities[1], installation: identities[2], transaction: identities[3],
                    receivedAtNanoseconds: 0,
                    resolve: { _, _ in XCTFail("foreign discovery"); throw WifiDiscoveryError.discovery },
                    makeWire: { _, _ in XCTFail("foreign HTTP"); throw WifiHTTPError.invalidRequest }, clock: { 0 })
                XCTFail("foreign offer")
            } catch { XCTAssertEqual(error as? WifiHandoffTransportError, .binding) }
        }
        for now: UInt64 in [0, 31_000_000_000] {
            do {
                _ = try await WifiHandoffConnector.prepare(offer: offer, reader: offer.reader,
                    storageGeneration: offer.storageGeneration, installation: offer.installation,
                    transaction: offer.transaction, receivedAtNanoseconds: 1_000_000_000,
                    resolve: { _, _ in XCTFail("expired discovery"); throw WifiDiscoveryError.discovery },
                    makeWire: { _, _ in XCTFail("expired HTTP"); throw WifiHTTPError.invalidRequest }, clock: { now })
                XCTFail("expired offer")
            } catch { XCTAssertEqual(error as? WifiHandoffTransportError, .expired) }
        }
    }
    @MainActor
    func testFailedInvalidAndLateDiscoveryNeverCreateHTTP() async throws {
        let offer = try offer()
        for mode in 0..<4 {
            let clock = ConnectorClock()
            do {
                _ = try await WifiHandoffConnector.prepare(offer: offer, reader: offer.reader,
                    storageGeneration: offer.storageGeneration, installation: offer.installation,
                    transaction: offer.transaction, receivedAtNanoseconds: 0,
                    resolve: { _, _ in
                        if mode == 0 { throw WifiDiscoveryError.discovery }
                        if mode == 1 { throw CancellationError() }
                        if mode == 2 { return Data([127, 0, 0, 1]) }
                        clock.set(30_000_000_000); return Data([192, 168, 4, 1])
                    }, makeWire: { _, _ in XCTFail("invalid discovery HTTP"); throw WifiHTTPError.invalidRequest },
                    clock: { clock.now() })
                XCTFail("failed discovery")
            } catch {
                switch mode {
                case 0: XCTAssertEqual(error as? WifiDiscoveryError, .discovery)
                case 1: XCTAssertTrue(error is CancellationError)
                case 2: XCTAssertEqual(error as? WifiHTTPError, .invalidRequest)
                default: XCTAssertEqual(error as? WifiHandoffTransportError, .expired)
                }
            }
        }
    }
    @MainActor
    func testActualTaskCancellationDuringResolutionAndWireCreation() async throws {
        for duringResolution in [true, false] {
            let offered = try offer(discovery: duringResolution)
            let wire = try ConnectorWire(offered)
            var creations = 0
            let task = Task {
                try await WifiHandoffConnector.prepare(offer: offered, reader: offered.reader,
                    storageGeneration: offered.storageGeneration, installation: offered.installation,
                    transaction: offered.transaction, receivedAtNanoseconds: 0,
                    resolve: { _, _ in
                        withUnsafeCurrentTask { $0?.cancel() }
                        return Data([192, 168, 4, 1])
                    }, makeWire: { _, _ in
                        creations += 1
                        withUnsafeCurrentTask { $0?.cancel() }
                        return wire
                    }, clock: { 0 })
            }
            do { _ = try await task.value; XCTFail("cancelled setup") }
            catch { XCTAssertTrue(error is CancellationError) }
            XCTAssertEqual(creations, duringResolution ? 0 : 1)
            let state = await wire.state(); XCTAssertEqual(state.1, !duringResolution)
        }
    }
    @MainActor
    func testExpiryDuringWireCreationClosesWire() async throws {
        let offer = try offer(discovery: false)
        let wire = try ConnectorWire(offer)
        let clock = ConnectorClock()
        do {
            _ = try await WifiHandoffConnector.prepare(offer: offer, reader: offer.reader,
                storageGeneration: offer.storageGeneration, installation: offer.installation,
                transaction: offer.transaction, receivedAtNanoseconds: 0,
                resolve: { _, _ in XCTFail("fixed offer discovery"); throw WifiDiscoveryError.discovery },
                makeWire: { _, _ in clock.set(30_000_000_000); return wire }, clock: { clock.now() })
            XCTFail("expired creation")
        } catch { XCTAssertEqual(error as? WifiHandoffTransportError, .expired) }
        let state = await wire.state(); XCTAssertTrue(state.1)
    }
}

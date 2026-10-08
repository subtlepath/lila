import Foundation
import XCTest
@testable import CompanionKit

private final class DiscoveryClock: @unchecked Sendable {
    private let lock = NSLock()
    private var value: UInt64 = 0
    func set(_ value: UInt64) { lock.withLock { self.value = value } }
    func now() -> UInt64 { lock.withLock { value } }
}
@MainActor
private final class DiscoveryDriverFixture: WifiDiscoveryDriver {
    var starts = 0, stops = 0
    var name = "", type = ""
    var port: UInt16 = 0
    var timeout: TimeInterval = 0
    var callback: (@MainActor (Result<Data, any Error>) -> Void)?
    var immediate: Result<Data, any Error>?
    var beforeCallback: (() -> Void)?
    private var started: CheckedContinuation<Void, Never>?
    func start(name: String, type: String, port: UInt16, timeout: TimeInterval,
               completion: @escaping @MainActor (Result<Data, any Error>) -> Void) {
        starts += 1; self.name = name; self.type = type; self.port = port; self.timeout = timeout
        callback = completion
        started?.resume(); started = nil
        if let immediate { beforeCallback?(); completion(immediate) }
    }
    func waitStarted() async {
        if starts != 0 { return }
        await withCheckedContinuation { started = $0 }
    }
    func stop() { stops += 1 }
}

final class WifiBonjourResolverTests: XCTestCase {
    private func offer(discovery: Bool = true, lifetime: UInt16 = 30) throws -> WifiHandoffOffer {
        var bytes = Data([1])
        for value: UInt8 in 1...5 { bytes.append(Data(repeating: value, count: 16)) }
        bytes.append(Data(0..<32))
        bytes.append(contentsOf: discovery ? [0, 0, 0, 0] : [192, 168, 4, 1])
        bytes.appendLittleEndian(8080, count: 2); bytes.appendLittleEndian(UInt64(lifetime), count: 2)
        return try WifiHandoffOffer(decoding: bytes)
    }
    func testDarwinSocketAddressValidation() {
        var socket = Data([16, 2, 0x1f, 0x90, 192, 168, 4, 1] + Array(repeating: 0, count: 8))
        XCTAssertEqual(wifiDiscoveryIPv4(socket, port: 8080), Data([192, 168, 4, 1]))
        XCTAssertNil(wifiDiscoveryIPv4(socket, port: 8081))
        for count in 0..<16 { XCTAssertNil(wifiDiscoveryIPv4(socket.prefix(count), port: 8080)) }
        for (at, value): (Int, UInt8) in [(0, 0), (0, 28), (1, 30), (4, 0), (4, 127), (4, 224)] {
            var invalid = socket; invalid[at] = value
            XCTAssertNil(wifiDiscoveryIPv4(invalid, port: 8080))
        }
        socket.append(0)
        XCTAssertNil(wifiDiscoveryIPv4(socket, port: 8080))
    }
    func testServiceBindingAndBoundedAddressSelection() {
        let expected = "lila-05050505050505050505050505050505"
        XCTAssertTrue(wifiDiscoveryMatches(name: expected, type: "_lila-sync._tcp.", domain: "local.", expectedName: expected))
        for (name, type, domain) in [("other", "_lila-sync._tcp.", "local."),
                                     (expected, "_http._tcp.", "local."),
                                     (expected, "_lila-sync._tcp.", "example.com.")] {
            XCTAssertFalse(wifiDiscoveryMatches(name: name, type: type, domain: domain, expectedName: expected))
        }
        let socket = Data([16, 2, 0x1f, 0x90, 192, 168, 4, 1] + Array(repeating: 0, count: 8))
        XCTAssertEqual(wifiDiscoveryResolvedAddress(port: 8080, addresses: [Data(), socket], expectedPort: 8080), Data([192, 168, 4, 1]))
        XCTAssertNil(wifiDiscoveryResolvedAddress(port: 8081, addresses: [socket], expectedPort: 8080))
        XCTAssertNil(wifiDiscoveryResolvedAddress(port: 8080, addresses: Array(repeating: Data(), count: 16) + [socket], expectedPort: 8080))
    }
    @MainActor
    func testSuccessAndFailuresStopDiscovery() async throws {
        for result: Result<Data, any Error> in [.success(Data([192, 168, 4, 1])),
                                               .success(Data([127, 0, 0, 1])),
                                               .success(Data([192, 168, 4])),
                                               .failure(WifiDiscoveryError.discovery),
                                               .failure(WifiDiscoveryError.resolution)] {
            let clock = DiscoveryClock(); clock.set(5_000_000_000)
            let driver = DiscoveryDriverFixture(); driver.immediate = result
            do {
                let address = try await WifiBonjourResolver.resolve(offer(), receivedAtNanoseconds: 0,
                    driver: driver, clock: { clock.now() })
                XCTAssertEqual(address, Data([192, 168, 4, 1]))
                guard case .success(let expected) = result else { XCTFail("unexpected success"); continue }
                XCTAssertEqual(address, expected)
            } catch {
                if case .success(let expected) = result, expected == Data([192, 168, 4, 1]) { XCTFail("valid resolution failed") }
                XCTAssertTrue(error is WifiDiscoveryError)
            }
            XCTAssertEqual(driver.starts, 1); XCTAssertEqual(driver.stops, 1)
            XCTAssertEqual(driver.name, "lila-05050505050505050505050505050505")
            XCTAssertEqual(driver.type, "_lila-sync._tcp."); XCTAssertEqual(driver.port, 8080)
            XCTAssertEqual(driver.timeout, 25)
            driver.callback?(.success(Data([192, 168, 4, 2])))
            XCTAssertEqual(driver.stops, 1)
        }
    }
    @MainActor
    func testExpiredBackwardClockAndFixedOfferDoNotBrowse() async throws {
        for now: UInt64 in [0, 31_000_000_000] {
            let clock = DiscoveryClock(); clock.set(now)
            let driver = DiscoveryDriverFixture()
            do {
                _ = try await WifiBonjourResolver.resolve(offer(), receivedAtNanoseconds: 1_000_000_000,
                    driver: driver, clock: { clock.now() }); XCTFail("expired")
            } catch { XCTAssertEqual(error as? WifiDiscoveryError, .expired) }
            XCTAssertEqual(driver.starts, 0)
        }
        let driver = DiscoveryDriverFixture()
        do {
            _ = try await WifiBonjourResolver.resolve(offer(discovery: false), receivedAtNanoseconds: 0,
                driver: driver, clock: { 0 }); XCTFail("fixed address")
        } catch { XCTAssertEqual(error as? WifiDiscoveryError, .invalidOffer) }
        XCTAssertEqual(driver.starts, 0); XCTAssertEqual(driver.stops, 0)
    }
    @MainActor
    func testLateResultDoesNotExtendOfferLifetime() async throws {
        let clock = DiscoveryClock()
        let driver = DiscoveryDriverFixture(); driver.immediate = .success(Data([192, 168, 4, 1]))
        driver.beforeCallback = { clock.set(30_000_000_000) }
        do {
            _ = try await WifiBonjourResolver.resolve(offer(), receivedAtNanoseconds: 0,
                driver: driver, clock: { clock.now() }); XCTFail("late resolution")
        } catch { XCTAssertEqual(error as? WifiDiscoveryError, .expired) }
        XCTAssertEqual(driver.stops, 1)
    }
    @MainActor
    func testTimerAndCancellationReleasePendingDiscovery() async throws {
        let timerDriver = DiscoveryDriverFixture()
        do {
            _ = try await WifiBonjourResolver.resolve(offer(lifetime: 1), receivedAtNanoseconds: 0,
                driver: timerDriver, clock: { 999_000_000 }); XCTFail("timeout")
        } catch { XCTAssertEqual(error as? WifiDiscoveryError, .expired) }
        XCTAssertEqual(timerDriver.starts, 1); XCTAssertEqual(timerDriver.stops, 1)
        let driver = DiscoveryDriverFixture()
        let offered = try offer()
        let task = Task { try await WifiBonjourResolver.resolve(offered, receivedAtNanoseconds: 0,
                                                               driver: driver, clock: { 0 }) }
        await driver.waitStarted()
        task.cancel()
        do { _ = try await task.value; XCTFail("cancelled") } catch { XCTAssertTrue(error is CancellationError) }
        XCTAssertEqual(driver.stops, 1)
        driver.callback?(.success(Data([192, 168, 4, 1])))
        XCTAssertEqual(driver.stops, 1)
        let beforeStart = DiscoveryDriverFixture()
        let cancelled = Task { try await WifiBonjourResolver.resolve(offered, receivedAtNanoseconds: 0,
                                                                     driver: beforeStart, clock: { 0 }) }
        cancelled.cancel()
        do { _ = try await cancelled.value; XCTFail("pre-cancelled") } catch { XCTAssertTrue(error is CancellationError) }
        XCTAssertEqual(beforeStart.starts, 0)
    }
}

import Foundation
import XCTest
@testable import CompanionKit

@MainActor
private final class HotspotDriverFixture: WifiHotspotDriver {
    var calls = 0, removals = 0
    var ssid: String?, password: String?
    var completion: (@MainActor (Error?) -> Void)?
    var immediate = false
    func apply(ssid: String, password: String, completion: @escaping @MainActor (Error?) -> Void) {
        calls += 1; self.ssid = ssid; self.password = password; self.completion = completion
        if immediate { completion(nil) }
    }
    func remove() { removals += 1 }
}
@MainActor
final class WifiHotspotJoinerTests: XCTestCase {
    private func network(mode: WifiNetworkMode = .hotspot) throws -> WifiNetworkOffer {
        var bytes = Data([1])
        for value: UInt8 in [1,2,3,4,5] { bytes.append(Data(repeating: value, count: 16)) }
        bytes.append(Data(repeating: 6, count: 32)); bytes.append(Data([0,0,0,0,0x90,0x1f,30,0]))
        return try WifiNetworkOffer(offer: WifiHandoffOffer(decoding: bytes), mode: mode,
            ssid: Data("lila-test".utf8), password: mode == .hotspot ? Data("0123456789abcdef".utf8) : Data())
    }
    func testAppliedConfigurationLeaseOwnsCleanupAndIgnoresDuplicateSuccess() async throws {
        let driver = HotspotDriverFixture(); driver.immediate = true
        let lease = try await WifiHotspotJoiner.apply(network: network(), receivedAtNanoseconds: 100,
                                                     driver: driver, clock: { 100 })
        XCTAssertEqual(driver.ssid, "lila-test"); XCTAssertEqual(driver.password, "0123456789abcdef")
        XCTAssertEqual(driver.removals, 0)
        driver.completion?(nil); XCTAssertEqual(driver.removals, 0)
        lease.close(); lease.close(); XCTAssertEqual(driver.removals, 1)
        driver.completion?(nil); XCTAssertEqual(driver.removals, 2)
    }
    func testSavedModeAndExpiredOrBackwardOffersNeverInvokeDriver() async throws {
        for scenario in 0..<3 {
            let driver = HotspotDriverFixture()
            do {
                _ = try await WifiHotspotJoiner.apply(network: network(mode: scenario == 0 ? .savedNetwork : .hotspot),
                    receivedAtNanoseconds: 100, driver: driver, clock: { scenario == 1 ? 30_000_000_100 : 99 })
                XCTFail()
            } catch {}
            XCTAssertEqual(driver.calls, 0); XCTAssertEqual(driver.removals, 0)
        }
    }
    func testCancellationRemovesConfigurationAndLateSuccessCannotRecreateLease() async throws {
        let network = try network(), driver = HotspotDriverFixture()
        let task = Task { @MainActor in
            try await WifiHotspotJoiner.apply(network: network, receivedAtNanoseconds: 0,
                                               driver: driver, clock: { 0 })
        }
        while driver.calls == 0 { await Task.yield() }
        task.cancel()
        do { _ = try await task.value; XCTFail() }
        catch is CancellationError {} catch { XCTFail("Unexpected error: \(error)") }
        XCTAssertEqual(driver.removals, 1)
        driver.completion?(nil); XCTAssertEqual(driver.removals, 2)
    }
    func testOriginalDeadlineTimerReleasesPendingJoinWithoutOsCallback() async throws {
        let driver = HotspotDriverFixture()
        do {
            _ = try await WifiHotspotJoiner.apply(network: network(), receivedAtNanoseconds: 0,
                driver: driver, clock: { 29_995_000_000 })
            XCTFail()
        } catch WifiHandoffTransportError.expired {} catch { XCTFail("Unexpected error: \(error)") }
        XCTAssertEqual(driver.calls, 1); XCTAssertEqual(driver.removals, 1)
        driver.completion?(nil); XCTAssertEqual(driver.removals, 2)
    }
    func testOsFailurePropagatesAndCleansTemporaryConfiguration() async throws {
        let network = try network(), driver = HotspotDriverFixture()
        let task = Task { @MainActor in
            try await WifiHotspotJoiner.apply(network: network, receivedAtNanoseconds: 0, driver: driver, clock: { 0 })
        }
        while driver.calls == 0 { await Task.yield() }
        driver.completion?(WifiHTTPError.invalidRequest)
        do { _ = try await task.value; XCTFail() }
        catch WifiHTTPError.invalidRequest {} catch { XCTFail("Unexpected error: \(error)") }
        XCTAssertEqual(driver.removals, 1)
    }
    func testOverlappingJoinIsBusyUntilFirstOperationIsReleased() async throws {
        let network = try network(), first = HotspotDriverFixture(), second = HotspotDriverFixture()
        let task = Task { @MainActor in
            try await WifiHotspotJoiner.apply(network: network, receivedAtNanoseconds: 0, driver: first, clock: { 0 })
        }
        while first.calls == 0 { await Task.yield() }
        do {
            _ = try await WifiHotspotJoiner.apply(network: network, receivedAtNanoseconds: 0, driver: second, clock: { 0 })
            XCTFail()
        } catch WifiHotspotJoinError.busy {} catch { XCTFail("Unexpected error: \(error)") }
        XCTAssertEqual(second.calls, 0)
        task.cancel()
        do { _ = try await task.value; XCTFail() } catch is CancellationError {} catch { XCTFail() }
        second.immediate = true
        let lease = try await WifiHotspotJoiner.apply(network: network, receivedAtNanoseconds: 0, driver: second, clock: { 0 })
        lease.close()
        XCTAssertEqual(second.calls, 1)
    }

}

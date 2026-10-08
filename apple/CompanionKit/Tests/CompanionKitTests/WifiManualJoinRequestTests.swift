import Foundation
import XCTest
@testable import CompanionKit

@MainActor
final class WifiManualJoinRequestTests: XCTestCase {
    private func network(_ mode: WifiNetworkMode = .hotspot) throws -> WifiNetworkOffer {
        var bytes = Data([1])
        for value: UInt8 in [1,2,3,4,5] { bytes.append(Data(repeating: value, count: 16)) }
        bytes.append(Data(repeating: 6, count: 32)); bytes.append(Data([0,0,0,0,0x90,0x1f,30,0]))
        return try WifiNetworkOffer(offer: WifiHandoffOffer(decoding: bytes), mode: mode,
            ssid: Data("lila-test".utf8), password: mode == .hotspot ? Data("0123456789abcdef".utf8) : Data())
    }
    func testGuidanceOmitsSavedPasswordAndConfirmationIsOneShot() async throws {
        let saved = try WifiManualJoinRequest(network: network(.savedNetwork), receivedAtNanoseconds: 0, clock: { 0 })
        XCTAssertEqual(saved.ssid, "lila-test"); XCTAssertNil(saved.password)
        let request = try WifiManualJoinRequest(network: network(), receivedAtNanoseconds: 0, clock: { 0 })
        XCTAssertEqual(request.password, "0123456789abcdef")
        let task = Task { @MainActor in try await request.waitForConfirmation() }
        while !request.awaitingConfirmation { await Task.yield() }
        request.confirm(); request.confirm()
        try await task.value
        do { try await request.waitForConfirmation(); XCTFail() }
        catch WifiManualJoinError.finished {} catch { XCTFail("Unexpected error: \(error)") }
    }
    func testCancelBeforeAndDuringWaitPreventsContinuation() async throws {
        let early = try WifiManualJoinRequest(network: network(), receivedAtNanoseconds: 0, clock: { 0 })
        early.cancel()
        do { try await early.waitForConfirmation(); XCTFail() } catch is CancellationError {} catch { XCTFail() }
        let request = try WifiManualJoinRequest(network: network(), receivedAtNanoseconds: 0, clock: { 0 })
        let task = Task { @MainActor in try await request.waitForConfirmation() }
        while !request.awaitingConfirmation { await Task.yield() }
        task.cancel()
        do { try await task.value; XCTFail() } catch is CancellationError {} catch { XCTFail() }
        request.confirm()
        XCTAssertFalse(request.awaitingConfirmation)
    }
    func testOriginalDeadlineExpiresWithoutUserInteraction() async throws {
        let request = try WifiManualJoinRequest(network: network(), receivedAtNanoseconds: 0, clock: { 29_995_000_000 })
        do { try await request.waitForConfirmation(); XCTFail() }
        catch WifiHandoffTransportError.expired {} catch { XCTFail("Unexpected error: \(error)") }
        request.confirm(); XCTAssertFalse(request.awaitingConfirmation)
    }
    func testUnrenderableSavedSsidAndBackwardClockAreRejected() async throws {
        let base = try network(.savedNetwork)
        let invalid = try WifiNetworkOffer(offer: base.offer, mode: .savedNetwork, ssid: Data([0xff]))
        XCTAssertThrowsError(try WifiManualJoinRequest(network: invalid, receivedAtNanoseconds: 0, clock: { 0 }))
        XCTAssertThrowsError(try WifiManualJoinRequest(network: base, receivedAtNanoseconds: 100, clock: { 99 }))
    }
    func testOverlappingWaitIsBusyAndCancelWinsConfirmationResumeRace() async throws {
        let request = try WifiManualJoinRequest(network: network(), receivedAtNanoseconds: 0, clock: { 0 })
        let task = Task { @MainActor in try await request.waitForConfirmation() }
        while !request.awaitingConfirmation { await Task.yield() }
        do { try await request.waitForConfirmation(); XCTFail() }
        catch WifiManualJoinError.busy {} catch { XCTFail("Unexpected error: \(error)") }
        request.confirm()
        request.cancel()
        do { try await task.value; XCTFail() }
        catch is CancellationError {} catch { XCTFail("Unexpected error: \(error)") }
    }

}

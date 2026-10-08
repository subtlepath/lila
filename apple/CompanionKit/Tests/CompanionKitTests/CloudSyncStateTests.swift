import Foundation
import XCTest
@testable import CompanionKit

final class CloudSyncStateTests: XCTestCase, @unchecked Sendable {
    func testRetryDeadlineSurvivesRestartAndAccountSwitchClearsIt() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("cloud.json")
        let store = try CloudSyncStateStore(url: url)
        let now = Date(timeIntervalSince1970: 1000), deadline = Date(timeIntervalSince1970: 1060)
        try await store.observeAccount("first")
        try await store.enable(confirmedAccount: "first", now: now)
        try await store.saveSerialization(Data([1]), account: "first")
        try await store.deferRetry(until: deadline, account: "first")
        try await store.deferRetry(until: now, account: "first")
        let reopened = try CloudSyncStateStore(url: url)
        let retained = await reopened.snapshot()
        XCTAssertFalse(retained.allowsForegroundResume(now: now))
        XCTAssertFalse(retained.enabled); XCTAssertEqual(retained.retryNotBefore, deadline)
        XCTAssertEqual(retained.serialization, Data([1]))
        do { try await reopened.enable(confirmedAccount: "first", now: now); XCTFail() }
        catch { XCTAssertEqual(error as? CloudSyncStateError, .retryDeferred(deadline)) }
        try await reopened.enable(confirmedAccount: "first", now: deadline)
        let resumed = await reopened.snapshot(); XCTAssertTrue(resumed.enabled); XCTAssertNil(resumed.retryNotBefore)
        XCTAssertTrue(resumed.allowsForegroundResume(now: deadline))
        try await reopened.deferRetry(until: deadline, account: "first")
        try await reopened.observeAccount("second")
        let switched = await reopened.snapshot(); XCTAssertNil(switched.retryNotBefore)
        XCTAssertFalse(switched.allowsForegroundResume(now: deadline))
        do { try await reopened.deferRetry(until: deadline, account: "first"); XCTFail() }
        catch { XCTAssertEqual(error as? CloudSyncStateError, .accountConfirmationRequired) }
        try await reopened.enable(confirmedAccount: "second", now: now)
    }
    func testAccountSwitchDisablesSyncAndClearsCheckpointWithoutReusingOldAccount() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("cloud.json")
        let store = try CloudSyncStateStore(url: url)
        let initial = await store.snapshot(); XCTAssertEqual(initial, CloudSyncState())
        try await store.observeAccount("first")
        do { try await store.saveSerialization(Data([1]), account: "first"); XCTFail() }
        catch { XCTAssertEqual(error as? CloudSyncStateError, .accountConfirmationRequired) }
        try await store.enable(confirmedAccount: "first")
        try await store.saveSerialization(Data([1, 2, 3]), account: "first")
        let reopened = try CloudSyncStateStore(url: url)
        let restored = await reopened.snapshot(); XCTAssertTrue(restored.enabled)
        XCTAssertEqual(restored.serialization, Data([1, 2, 3]))
        let unchanged = try await reopened.observeAccount("first"); XCTAssertFalse(unchanged)
        try await reopened.observeAccount("second")
        let switched = await reopened.snapshot(); XCTAssertFalse(switched.enabled); XCTAssertNil(switched.serialization)
        do { try await reopened.enable(confirmedAccount: "first"); XCTFail() }
        catch { XCTAssertEqual(error as? CloudSyncStateError, .accountConfirmationRequired) }
        do { try await reopened.saveSerialization(Data([9]), account: "first"); XCTFail() }
        catch { XCTAssertEqual(error as? CloudSyncStateError, .accountConfirmationRequired) }
        try await reopened.enable(confirmedAccount: "second")
        try await reopened.saveSerialization(Data([4]), account: "second")
        try await reopened.disable()
        let paused = await reopened.snapshot(); XCTAssertFalse(paused.enabled); XCTAssertEqual(paused.serialization, Data([4]))
        try await reopened.observeAccount(nil)
        let signedOut = await reopened.snapshot(); XCTAssertNil(signedOut.account); XCTAssertNil(signedOut.serialization)
    }
    func testCorruptStateIsNotSilentlyResetAndFailedWritePreservesMemory() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let url = root.appendingPathComponent("cloud.json")
        try Data("broken".utf8).write(to: url)
        XCTAssertThrowsError(try CloudSyncStateStore(url: url))
        try FileManager.default.removeItem(at: url)
        let store = try CloudSyncStateStore(url: url)
        for account in ["", "bad\0account", String(repeating: "x", count: 1025)] {
            do { try await store.observeAccount(account); XCTFail() }
            catch { XCTAssertEqual(error as? CloudSyncStateError, .invalidAccount) }
        }
        try await store.observeAccount("first")
        try FileManager.default.removeItem(at: url)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        do { try await store.enable(confirmedAccount: "first"); XCTFail("Write failure ignored") } catch {}
        let retained = await store.snapshot(); XCTAssertFalse(retained.enabled); XCTAssertEqual(retained.account, "first")
    }
}

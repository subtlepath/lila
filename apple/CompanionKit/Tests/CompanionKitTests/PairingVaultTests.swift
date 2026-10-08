import Foundation
import XCTest
@testable import CompanionKit

private final class CredentialFixture: CredentialStorage, @unchecked Sendable {
    private let lock = NSLock()
    private var values: [String: Data] = [:]
    func load(_ account: String) throws -> Data? {
        lock.lock(); defer { lock.unlock() }; return values[account]
    }
    func insert(_ account: String, data: Data) throws -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard values[account] == nil else { return false }
        values[account] = data; return true
    }
    func remove(_ account: String) throws {
        lock.lock(); defer { lock.unlock() }; values.removeValue(forKey: account)
    }
    func corrupt(_ account: String, data: Data) {
        lock.lock(); defer { lock.unlock() }; values[account] = data
    }
}
private final class CredentialRandomFixture: @unchecked Sendable {
    private let lock = NSLock()
    private var value: UInt8 = 0
    func bytes(_ count: Int) -> Data {
        lock.lock(); defer { lock.unlock() }; value += 1
        return Data(repeating: value, count: count)
    }
}

final class PairingVaultTests: XCTestCase, @unchecked Sendable {
    func testRestartLostRegistrationResponseAndReaderIsolation() async throws {
        let storage = CredentialFixture(), random = CredentialRandomFixture()
        let vault = PairingVault(storage: storage, random: { random.bytes($0) })
        let reader = Data(repeating: 9, count: 16), other = Data(repeating: 10, count: 16)
        let first = try await vault.preparePairing(for: reader)
        let second = try await vault.preparePairing(for: other)
        XCTAssertEqual(first.authenticationPayload.count, 48)
        XCTAssertEqual(first.installation, second.installation)
        XCTAssertNotEqual(first.secret, second.secret)
        let reopened = PairingVault(storage: storage, random: { random.bytes($0) })
        let retry = try await reopened.preparePairing(for: reader)
        XCTAssertEqual(retry.authenticationPayload, first.authenticationPayload)
        try await reopened.forgetReader(reader)
        let forgotten = try await reopened.credential(for: reader)
        XCTAssertNil(forgotten)
        let retained = try await reopened.credential(for: other)
        XCTAssertEqual(retained?.authenticationPayload, second.authenticationPayload)
    }
    func testConcurrentVaultsKeepOneDurableCredential() async throws {
        let storage = CredentialFixture(), random = CredentialRandomFixture()
        let first = PairingVault(storage: storage, random: { random.bytes($0) })
        let second = PairingVault(storage: storage, random: { random.bytes($0) })
        let reader = Data(repeating: 9, count: 16)
        let payloads = try await withThrowingTaskGroup(of: Data.self) { group in
            for index in 0 ..< 20 {
                group.addTask { try await (index % 2 == 0 ? first : second).preparePairing(for: reader).authenticationPayload }
            }
            var results: [Data] = []; results.reserveCapacity(20)
            for try await payload in group { results.append(payload) }
            return results
        }
        XCTAssertEqual(Set(payloads).count, 1)
    }
    func testCorruptRecordsAndMissingInstallationFailClosed() async throws {
        let storage = CredentialFixture()
        let reader = Data(repeating: 9, count: 16)
        let account = "reader." + String(repeating: "09", count: 16)
        storage.corrupt(account, data: Data(repeating: 1, count: 49))
        let vault = PairingVault(storage: storage, random: { Data(repeating: 2, count: $0) })
        do { _ = try await vault.preparePairing(for: reader); XCTFail("Missing identity replaced") }
        catch { XCTAssertEqual(error as? CredentialError, .corruptRecord) }
        XCTAssertNil(try storage.load("installation"))
        storage.corrupt("installation", data: Data(repeating: 0, count: 16))
        do { _ = try await vault.installationIdentity(); XCTFail("Corrupt identity replaced") }
        catch { XCTAssertEqual(error as? CredentialError, .corruptRecord) }
    }
    func testInvalidReaderAndRandomOutputAreRejected() async throws {
        let vault = PairingVault(storage: CredentialFixture(), random: { Data(repeating: 0, count: $0) })
        do { _ = try await vault.preparePairing(for: Data()); XCTFail("Invalid reader accepted") }
        catch { XCTAssertEqual(error as? CredentialError, .invalidIdentity) }
        do { _ = try await vault.preparePairing(for: Data(repeating: 1, count: 16)); XCTFail("Zero random identity accepted") }
        catch { XCTAssertEqual(error as? CredentialError, .invalidRandom) }
    }
}

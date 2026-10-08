import Foundation
#if canImport(Security)
import Security
#endif

public enum CredentialError: Error, Equatable, Sendable {
    case invalidIdentity, corruptRecord, storage(Int32), unavailable, invalidRandom
}

public protocol CredentialStorage: Sendable {
    func load(_ account: String) throws -> Data?
    func insert(_ account: String, data: Data) throws -> Bool
    func remove(_ account: String) throws
}

// Credentials intentionally have no Codable conformance or library/cloud representation.
public struct PairingCredential: Sendable {
    public let installation: Data
    public let secret: Data
    public var authenticationPayload: Data {
        var bytes = Data(); bytes.reserveCapacity(48)
        bytes.append(installation); bytes.append(secret)
        return bytes
    }
}

public actor PairingVault {
    private let storage: any CredentialStorage
    private let random: @Sendable (Int) throws -> Data
    public init(storage: any CredentialStorage, random: @escaping @Sendable (Int) throws -> Data = installationRandom) {
        self.storage = storage; self.random = random
    }
    public func installationIdentity() throws -> Data {
        if let existing = try storage.load("installation") { return try identity(existing) }
        let created = try random(16)
        guard created.count == 16, created.contains(where: { $0 != 0 }) else { throw CredentialError.invalidRandom }
        _ = try storage.insert("installation", data: created)
        guard let stored = try storage.load("installation") else { throw CredentialError.corruptRecord }
        return try identity(stored)
    }
    public func credential(for reader: Data) throws -> PairingCredential? {
        let account = try readerAccount(reader)
        guard let record = try storage.load(account) else { return nil }
        guard let storedIdentity = try storage.load("installation") else { throw CredentialError.corruptRecord }
        let installation = try identity(storedIdentity)
        guard record.count == 49, record.first == 1,
              Data(record.dropFirst().prefix(16)) == installation else { throw CredentialError.corruptRecord }
        let secret = Data(record.suffix(32))
        guard secret.contains(where: { $0 != 0 }) else { throw CredentialError.corruptRecord }
        return PairingCredential(installation: installation, secret: secret)
    }
    public func preparePairing(for reader: Data) throws -> PairingCredential {
        if let existing = try credential(for: reader) { return existing }
        let installation = try installationIdentity()
        let secret = try random(32)
        guard secret.count == 32, secret.contains(where: { $0 != 0 }) else { throw CredentialError.invalidRandom }
        var record = Data([1]); record.reserveCapacity(49)
        record.append(installation); record.append(secret)
        _ = try storage.insert(readerAccount(reader), data: record)
        guard let stored = try credential(for: reader) else { throw CredentialError.corruptRecord }
        return stored
    }
    public func forgetReader(_ reader: Data) throws { try storage.remove(readerAccount(reader)) }
    private func identity(_ bytes: Data) throws -> Data {
        guard bytes.count == 16, bytes.contains(where: { $0 != 0 }) else { throw CredentialError.corruptRecord }
        return bytes
    }
    private func readerAccount(_ reader: Data) throws -> String {
        guard reader.count == 16, reader.contains(where: { $0 != 0 }) else { throw CredentialError.invalidIdentity }
        return "reader." + reader.map { String(format: "%02x", $0) }.joined()
    }
}

public func installationRandom(_ count: Int) throws -> Data {
#if canImport(Security)
    guard count > 0 else { throw CredentialError.invalidRandom }
    var bytes = Data(count: count)
    let status = bytes.withUnsafeMutableBytes { buffer in
        SecRandomCopyBytes(kSecRandomDefault, count, buffer.baseAddress!)
    }
    guard status == errSecSuccess else { throw CredentialError.storage(status) }
    return bytes
#else
    throw CredentialError.unavailable
#endif
}

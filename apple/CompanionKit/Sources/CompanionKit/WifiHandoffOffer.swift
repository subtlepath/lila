import Foundation

public struct WifiHandoffOffer: Equatable, Sendable {
    public static let encodedSize = 121
    public let reader: Data
    public let storageGeneration: Data
    public let installation: Data
    public let transaction: Data
    public let session: Data
    public let key: Data
    public let address: Data
    public let port: UInt16
    public let lifetimeSeconds: UInt16

    public init(decoding bytes: Data) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        var input = ByteReader(bytes)
        guard try input.number(1) == 1 else { throw ProtocolError.version }
        reader = try input.take(16)
        storageGeneration = try input.take(16)
        installation = try input.take(16)
        transaction = try input.take(16)
        session = try input.take(16)
        key = try input.take(32)
        address = try input.take(4)
        port = UInt16(try input.number(2))
        lifetimeSeconds = UInt16(try input.number(2))
        guard [reader, storageGeneration, installation, transaction, session, key].allSatisfy({ $0.contains(where: { $0 != 0 }) }),
              (address.allSatisfy { $0 == 0 } || (address[0] != 0 && address[0] != 127 && address[0] < 224)), port != 0,
              lifetimeSeconds > 0, lifetimeSeconds <= 120 else { throw ProtocolError.value }
    }

    public static let discoveryServiceType = "_lila-sync._tcp."
    public var discoveryName: String { "lila-" + session.map { String(format: "%02x", $0) }.joined() }

    public var requiresDiscovery: Bool { address.allSatisfy { $0 == 0 } }

    public var encoded: Data {
        var bytes = Data([1])
        bytes.reserveCapacity(Self.encodedSize)
        for identity in [reader, storageGeneration, installation, transaction, session] { bytes.append(identity) }
        bytes.append(key); bytes.append(address)
        bytes.appendLittleEndian(UInt64(port), count: 2)
        bytes.appendLittleEndian(UInt64(lifetimeSeconds), count: 2)
        return bytes
    }

    public func matches(reader: Data, storageGeneration: Data, installation: Data, transaction: Data) -> Bool {
        self.reader == reader && self.storageGeneration == storageGeneration &&
        self.installation == installation && self.transaction == transaction
    }
}

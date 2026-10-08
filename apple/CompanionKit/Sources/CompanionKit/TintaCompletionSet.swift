import Foundation

public struct TintaCompletionSet: Equatable, Sendable {
    public enum Kind: UInt8, Sendable { case lessons = 1, readings = 2 }
    public let kind: Kind
    public let identities: Set<UInt32>
    public init(kind: Kind, identities: Set<UInt32>) throws {
        guard identities.count <= 65535, !identities.contains(0), !identities.contains(UInt32.max) else {
            throw ProtocolError.value
        }
        self.kind = kind; self.identities = identities
    }
    public var encoded: Data {
        var bytes = Data("TCS1".utf8)
        bytes.reserveCapacity(16 + identities.count * 4)
        bytes.append(contentsOf: [kind.rawValue, 0, 0, 0])
        bytes.appendLittleEndian(UInt64(identities.count), count: 4)
        for identity in identities.sorted() { bytes.appendLittleEndian(UInt64(identity), count: 4) }
        bytes.appendLittleEndian(UInt64(legacyCRC32(bytes)), count: 4)
        return bytes
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count >= 16 else { throw ProtocolError.value }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data("TCS1".utf8),
              let kind = Kind(rawValue: UInt8(try reader.number(1))),
              try reader.number(3) == 0 else { throw ProtocolError.value }
        let count = Int(try reader.number(4))
        guard count <= 65535, bytes.count == 16 + count * 4 else { throw ProtocolError.value }
        var identities = Set<UInt32>(); identities.reserveCapacity(count)
        var previous: UInt32 = 0
        for _ in 0..<count {
            let identity = UInt32(try reader.number(4))
            guard identity > previous, identity != UInt32.max else { throw ProtocolError.value }
            identities.insert(identity); previous = identity
        }
        guard try reader.number(4) == UInt64(legacyCRC32(Data(bytes.dropLast(4)))) else { throw ProtocolError.value }
        try self.init(kind: kind, identities: identities)
    }
}

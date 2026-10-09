import Foundation

/// Immutable ownership and content contract; no caller-supplied deletion path.
public struct ContentRemovalRequest: Equatable, Sendable {
    public static let encodedSize = 115
    public let transaction: Data
    public let owner: Data
    public let generation: Data
    public let manifest: ContentManifest
    public init(transaction: Data, owner: Data, generation: Data, manifest: ContentManifest) throws {
        guard [transaction, owner, generation].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }),
              manifest.length > 0, manifest.content.digest.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        let family = manifest.logicalIdentity.contains(where: { $0 != 0 })
        let valid: Bool
        switch manifest.kind {
        case .epub: valid = manifest.formatVersion <= 1 && !family
        case .course: valid = manifest.formatVersion == 1 && family
        case .font: valid = (manifest.formatVersion == 1 || manifest.formatVersion == 4) && !family
        case .dictionary: valid = manifest.formatVersion == 1 && !family
        case .firmware: valid = false
        }
        guard valid else { throw ProtocolError.value }
        self.transaction = transaction; self.owner = owner; self.generation = generation; self.manifest = manifest
    }
    public var encoded: Data {
        var bytes = Data([0x4c, 0x52, 0x4d, 1]); bytes.reserveCapacity(Self.encodedSize)
        bytes.append(transaction); bytes.append(owner); bytes.append(generation); bytes.append(manifest.encoded)
        return bytes
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x4c, 0x52, 0x4d, 1]) else { throw ProtocolError.version }
        try self.init(transaction: reader.take(16), owner: reader.take(16), generation: reader.take(16),
            manifest: ContentManifest(decoding: reader.take(63)))
    }
}

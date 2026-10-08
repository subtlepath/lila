import Foundation

// Bind the immutable content contract to the transaction before staging or recovery.
public struct TransferDeclaration: Equatable, Sendable {
    public static let encodedSize = 162
    public let manifest: ContentManifest
    public let state: TransferState
    public init(manifest: ContentManifest, state: TransferState) throws {
        guard state.phase == .receiving, state.durableOffset == 0,
              manifest.length > 0, manifest.length == state.length,
              manifest.content.digest == state.contentHash, manifest.logicalIdentity.count == 16 else {
            throw ProtocolError.value
        }
        if manifest.kind == .course {
            guard manifest.formatVersion > 0, manifest.logicalIdentity.contains(where: { $0 != 0 }) else {
                throw ProtocolError.value
            }
        } else {
            guard !manifest.logicalIdentity.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        }
        self.manifest = manifest; self.state = state
    }
    public var encoded: Data {
        var bytes = Data(); bytes.reserveCapacity(Self.encodedSize)
        bytes.append(manifest.encoded); bytes.append(state.encoded())
        return bytes
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        try self.init(manifest: ContentManifest(decoding: Data(bytes.prefix(63))),
                      state: TransferState(decoding: Data(bytes.dropFirst(63))))
    }
}

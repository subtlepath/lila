import Foundation

/// Exact original-pack confirmation for a frozen learner review and one transfer.
public struct CourseBaselineImportRequest: Equatable, Sendable {
    public static let encodedSize = 155
    private static let prefix = Data([0x54, 0x43, 0x42, 0x49, 1, 1, 0, 0])
    public let generation: Data
    public let owner: Data
    public let transaction: Data
    public let manifest: ContentManifest
    public let reviewHash: Data

    public init(generation: Data, owner: Data, transaction: Data,
                manifest: ContentManifest, reviewHash: Data) throws {
        guard [generation, owner, transaction].allSatisfy({
            $0.count == 16 && $0.contains(where: { $0 != 0 })
        }), reviewHash.count == 32, reviewHash.contains(where: { $0 != 0 }),
        manifest.kind == .course, manifest.formatVersion == 1,
        manifest.length > 0, manifest.length <= UInt64(UInt32.max),
        manifest.logicalIdentity.contains(where: { $0 != 0 }),
        manifest.content.digest.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        self.generation = generation; self.owner = owner; self.transaction = transaction
        self.manifest = manifest; self.reviewHash = reviewHash
    }

    public func frame(requestID: UInt32) throws -> ControlFrame {
        try ControlFrame(command: .beginCourseBaseline, requestID: requestID, payload: encoded)
    }

    public var encoded: Data {
        var bytes = Self.prefix
        bytes.reserveCapacity(Self.encodedSize)
        bytes.append(generation); bytes.append(owner); bytes.append(transaction)
        bytes.append(manifest.encoded); bytes.append(reviewHash)
        bytes.appendLittleEndian(UInt64(legacyCRC32(bytes)), count: 4)
        return bytes
    }

    public init(decoding bytes: Data) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(8) == Self.prefix else { throw ProtocolError.version }
        let generation = try reader.take(16), owner = try reader.take(16), transaction = try reader.take(16)
        let manifest = try ContentManifest(decoding: reader.take(63)), reviewHash = try reader.take(32)
        guard try reader.number(4) == UInt64(legacyCRC32(Data(bytes.prefix(151)))) else {
            throw ProtocolError.value
        }
        try self.init(generation: generation, owner: owner, transaction: transaction,
                      manifest: manifest, reviewHash: reviewHash)
    }

    public func matches(generation: Data, owner: Data, reviewed: Data, transfer: TransferDeclaration) -> Bool {
        self.generation == generation && self.owner == owner && reviewHash == reviewed &&
        self.generation == transfer.state.storageGeneration && self.owner == transfer.state.owner &&
        transaction == transfer.state.transaction && manifest == transfer.manifest
    }
}

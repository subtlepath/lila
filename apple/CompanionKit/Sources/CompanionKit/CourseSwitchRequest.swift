import Foundation

/// Consent for exact old and new pack bytes, one transfer, and one SD generation.
public struct CourseSwitchRequest: Equatable, Sendable {
    public static let encodedSize = 132
    public let generation: Data
    public let transaction: Data
    public let previousCourse: Data
    public let nextCourse: Data
    public let previousHash: Data
    public let nextHash: Data

    public init(generation: Data, transaction: Data, previousCourse: Data, nextCourse: Data,
                previousHash: Data, nextHash: Data) throws {
        guard [generation, transaction, previousCourse, nextCourse].allSatisfy({
            $0.count == 16 && $0.contains(where: { $0 != 0 })
        }), [previousHash, nextHash].allSatisfy({
            $0.count == 32 && $0.contains(where: { $0 != 0 })
        }), previousCourse != nextCourse, previousHash != nextHash else { throw ProtocolError.value }
        self.generation = generation; self.transaction = transaction
        self.previousCourse = previousCourse; self.nextCourse = nextCourse
        self.previousHash = previousHash; self.nextHash = nextHash
    }
    /// Initial authorization; durable recovery must retain the saved declarations.
    public func matches(generation currentGeneration: Data, current: ContentManifest,
                        next: TransferDeclaration) -> Bool {
        current.kind == .course && current.length > 0 && current.formatVersion > 0 &&
        next.manifest.kind == .course && generation == currentGeneration &&
        generation == next.state.storageGeneration && transaction == next.state.transaction &&
        previousCourse == current.logicalIdentity && previousHash == current.content.digest &&
        nextCourse == next.manifest.logicalIdentity && nextHash == next.manifest.content.digest
    }
    public var bytes: Data {
        var output = Data([0x54, 0x43, 0x53, 1])
        output.reserveCapacity(Self.encodedSize)
        output.append(generation); output.append(transaction)
        output.append(previousCourse); output.append(nextCourse)
        output.append(previousHash); output.append(nextHash)
        return output
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x54, 0x43, 0x53, 1]) else { throw ProtocolError.version }
        try self.init(generation: reader.take(16), transaction: reader.take(16),
                      previousCourse: reader.take(16), nextCourse: reader.take(16),
                      previousHash: reader.take(32), nextHash: reader.take(32))
    }
}

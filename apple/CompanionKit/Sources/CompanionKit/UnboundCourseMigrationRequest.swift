import Foundation

/// Explicit course assignment for a frozen global review; validation and recovery remain separate.
public struct UnboundCourseMigrationRequest: Equatable, Sendable {
    public static let encodedSize = CourseBaselineImportRequest.encodedSize
    private static let prefix = Data([0x54, 0x43, 0x55, 0x4d, 1, 1, 0, 0])
    private let original: CourseBaselineImportRequest
    public var generation: Data { original.generation }
    public var owner: Data { original.owner }
    public var transaction: Data { original.transaction }
    public var originalPack: ContentManifest { original.manifest }
    public var reviewHash: Data { original.reviewHash }

    public init(generation: Data, owner: Data, transaction: Data, originalPack: ContentManifest,
                review: CourseBaselineReview) throws {
        guard !review.isolated, review.generation == generation, review.course == originalPack.logicalIdentity else {
            throw ProtocolError.value
        }
        original = try CourseBaselineImportRequest(generation: generation, owner: owner, transaction: transaction,
            manifest: originalPack, reviewHash: review.hash)
    }
    public var encoded: Data {
        var bytes = original.encoded
        bytes.replaceSubrange(0..<8, with: Self.prefix)
        bytes.removeLast(4)
        bytes.appendLittleEndian(UInt64(legacyCRC32(bytes)), count: 4)
        return bytes
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        guard bytes.prefix(8) == Self.prefix else { throw ProtocolError.version }
        var tail = ByteReader(Data(bytes.suffix(4)))
        guard try tail.number(4) == UInt64(legacyCRC32(Data(bytes.prefix(151)))) else { throw ProtocolError.value }
        var archive = bytes
        archive.replaceSubrange(0..<8, with: Data([0x54, 0x43, 0x42, 0x49, 1, 1, 0, 0]))
        archive.removeLast(4)
        archive.appendLittleEndian(UInt64(legacyCRC32(archive)), count: 4)
        original = try CourseBaselineImportRequest(decoding: archive)
    }
    public func matches(review: CourseBaselineReview, reader: Data, generation: Data, owner: Data,
                        transfer: TransferDeclaration) -> Bool {
        !review.isolated && review.reader == reader && review.generation == generation &&
        review.course == originalPack.logicalIdentity &&
        original.matches(generation: generation, owner: owner, reviewed: review.hash, transfer: transfer)
    }
}

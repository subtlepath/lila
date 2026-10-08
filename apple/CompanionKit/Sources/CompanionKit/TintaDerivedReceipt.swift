import Foundation

public struct TintaDerivedReceipt: Equatable, Sendable {
    public struct File: Equatable, Sendable {
        public let length: UInt64
        public let hash: Data
    }
    public let course: Data
    public let packHash: Data
    public let frontierHash: Data
    public let snapshotIdentity: Data
    public let storageGeneration: Data
    public let studyDay: UInt16
    public let revision: UInt64
    public let files: [File]

    public init(decoding bytes: Data) throws {
        guard bytes.count == 332 else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data("TDS1".utf8) else { throw ProtocolError.version }
        course = try reader.take(16)
        packHash = try reader.take(32)
        frontierHash = try reader.take(32)
        snapshotIdentity = try reader.take(16)
        storageGeneration = try reader.take(16)
        studyDay = UInt16(try reader.number(2))
        guard try reader.number(2) == 0 else { throw ProtocolError.value }
        revision = try reader.number(8)
        guard revision > 0, [course, packHash, frontierHash, snapshotIdentity, storageGeneration]
            .allSatisfy({ $0.contains(where: { $0 != 0 }) }) else { throw ProtocolError.value }
        var receipts: [File] = []; receipts.reserveCapacity(5)
        for index in 0 ..< 5 {
            let length = try reader.number(8), hash = try reader.take(32)
            guard hash.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
            let valid: Bool
            switch index {
            case 0: valid = (1024 ... 525296).contains(length) && (length - 1024) % 16 == 0
            case 1: valid = length == 0
            case 2, 3: valid = (16 ... 262156).contains(length) && (length - 16) % 4 == 0
            default: valid = (4 ... UInt64(UInt32.max)).contains(length) && (length - 4) % 12 == 0
            }
            guard valid else { throw ProtocolError.value }
            receipts.append(File(length: length, hash: hash))
        }
        guard try reader.number(4) == UInt64(legacyCRC32(bytes.prefix(328))) else { throw VaultError.integrity }
        files = receipts
    }
}

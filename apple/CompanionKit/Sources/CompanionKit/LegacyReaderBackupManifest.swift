import Foundation

public struct LegacyReaderBackupManifest: Equatable, Sendable {
    public static let encodedSize = 436
    private static let roles: [LegacyBackupRole] = [.reviews, .items, .profile, .lessons, .readings, .starred, .usage, .days, .session]
    public let transaction: Data
    public let manifest: LegacyBackupManifest
    public init(transaction: Data, manifest: LegacyBackupManifest) throws {
        try manifest.validate()
        guard transaction.count == 16, transaction.contains(where: { $0 != 0 }),
              manifest.files.allSatisfy({ $0.length <= UInt32.max && $0.id.digest.contains(where: { $0 != 0 }) &&
                  ($0.role != .reviews || $0.length <= 16 * 1024 * 1024) }) else { throw ProtocolError.value }
        self.transaction = transaction; self.manifest = manifest
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x54, 0x4c, 0x42, 0x31]) else { throw ProtocolError.version }
        let device = try reader.take(16), generation = try reader.take(16), course = try reader.take(16)
        let transaction = try reader.take(16), mask = try reader.number(2)
        guard mask & ~UInt64(0x1ff) == 0, mask & 7 == 7, try reader.number(2) == 0 else { throw ProtocolError.value }
        var files: [LegacyBackupFile] = []; files.reserveCapacity(9)
        for (index, role) in Self.roles.enumerated() {
            let length = try reader.number(8), hash = try reader.take(32)
            if mask & (1 << index) == 0 {
                guard length == 0, hash == Data(count: 32) else { throw ProtocolError.value }
            } else {
                let id = try ContentID(hash.map { String(format: "%02x", $0) }.joined())
                files.append(LegacyBackupFile(role: role, id: id, length: length))
            }
        }
        guard try reader.number(4) == UInt64(legacyCRC32(bytes.prefix(432))) else { throw ProtocolError.value }
        try self.init(transaction: transaction, manifest: LegacyBackupManifest(reader: device, generation: generation,
            course: course, files: files))
    }
    public var bytes: Data {
        var result = Data([0x54, 0x4c, 0x42, 0x31]); result.reserveCapacity(Self.encodedSize)
        result.append(manifest.reader); result.append(manifest.generation); result.append(manifest.course); result.append(transaction)
        var mask: UInt64 = 0
        for (index, role) in Self.roles.enumerated() where manifest.files.contains(where: { $0.role == role }) { mask |= 1 << index }
        result.appendLittleEndian(mask, count: 2); result.append(contentsOf: [0, 0])
        for role in Self.roles {
            let file = manifest.files.first(where: { $0.role == role })
            result.appendLittleEndian(file?.length ?? 0, count: 8)
            result.append(file?.id.digest ?? Data(count: 32))
        }
        result.appendLittleEndian(UInt64(legacyCRC32(result)), count: 4)
        return result
    }
}

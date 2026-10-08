import Foundation

public struct LegacyBackupRequest: Equatable, Sendable {
    public enum Operation: UInt8, Sendable { case capture = 1, manifest, file, close }
    public let operation: Operation
    public let role: LegacyBackupRole?
    public let bound: Bool
    public let course: Data
    public let transaction: Data
    public let generation: Data
    public let offset: UInt32
    public let count: UInt16
    private static let roles: [LegacyBackupRole] = [.reviews, .items, .profile, .lessons, .readings, .starred, .usage, .days, .session]

    public init(operation: Operation, role: LegacyBackupRole? = nil, bound: Bool,
                course: Data, transaction: Data, generation: Data, offset: UInt32 = 0, count: UInt16 = 0) throws {
        guard [course, transaction, generation].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }),
              operation == .file ? (role != nil && count > 0 && count <= 768) : (role == nil && offset == 0 && count == 0)
        else { throw ProtocolError.value }
        self.operation = operation; self.role = role; self.bound = bound
        self.course = course; self.transaction = transaction; self.generation = generation
        self.offset = offset; self.count = count
    }
    public var bytes: Data {
        var result = Data([0x54, 0x4c, 0x52, 1, operation.rawValue,
                           role.map { UInt8(Self.roles.firstIndex(of: $0)!) } ?? 0xff, bound ? 1 : 0, 0])
        result.reserveCapacity(64)
        result.append(course); result.append(transaction); result.append(generation)
        result.appendLittleEndian(UInt64(offset), count: 4)
        result.appendLittleEndian(UInt64(count), count: 2)
        result.append(contentsOf: [0, 0])
        return result
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == 64 else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x54, 0x4c, 0x52, 1]) else { throw ProtocolError.version }
        let operationByte = UInt8(try reader.number(1)), roleByte = UInt8(try reader.number(1)), boundByte = UInt8(try reader.number(1))
        guard let operation = Operation(rawValue: operationByte), boundByte <= 1, UInt8(try reader.number(1)) == 0,
              roleByte == 0xff || roleByte < Self.roles.count else { throw ProtocolError.value }
        let course = try reader.take(16), transaction = try reader.take(16), generation = try reader.take(16)
        let offset = UInt32(try reader.number(4)), count = UInt16(try reader.number(2))
        guard try reader.number(2) == 0 else { throw ProtocolError.value }
        try self.init(operation: operation, role: roleByte == 0xff ? nil : Self.roles[Int(roleByte)], bound: boundByte == 1,
                      course: course, transaction: transaction, generation: generation, offset: offset, count: count)
    }
}

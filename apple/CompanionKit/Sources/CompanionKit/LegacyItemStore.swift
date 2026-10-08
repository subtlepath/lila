import Foundation

public enum LegacyItemStoreError: Error, Equatable, Sendable {
    case invalidHeader, ambiguousHeader, recoveryRequired, truncatedRecords, invalidRecord(Int), duplicateUID(UInt32)
}
public struct LegacyItemStore: Sendable {
    public static func encodeSnapshot(items: [UInt32: ScheduledItem], studyDay: UInt16,
                                      newItems: UInt16, reviews: UInt16) throws -> Data {
        guard items.count <= 0x7fff else { throw ProtocolError.value }
        for (uid, item) in items {
            var record = ByteReader(item.bytes)
            guard try record.number(4) == UInt64(uid), uid > 0, uid < UInt32.max else {
                throw ProtocolError.value
            }
        }
        func header(sequence: UInt32) -> Data {
            var bytes = Data("TIS1".utf8)
            bytes.reserveCapacity(80)
            bytes.appendLittleEndian(1, count: 2); bytes.appendLittleEndian(80, count: 2)
            bytes.appendLittleEndian(UInt64(sequence), count: 4)
            bytes.appendLittleEndian(UInt64(items.count), count: 4)
            bytes.appendLittleEndian(0, count: 4)
            bytes.appendLittleEndian(UInt64(studyDay), count: 2)
            bytes.appendLittleEndian(UInt64(newItems), count: 2)
            bytes.appendLittleEndian(UInt64(reviews), count: 2)
            bytes.append(Data(count: 50))
            bytes.appendLittleEndian(UInt64(legacyCRC32(bytes)), count: 4)
            return bytes
        }
        var output = Data(count: 1024)
        output.reserveCapacity(1024 + items.count * 16)
        output.replaceSubrange(0..<80, with: header(sequence: 1))
        output.replaceSubrange(512..<592, with: header(sequence: 2))
        for uid in items.keys.sorted() { output.append(items[uid]!.bytes) }
        return output
    }

    public let sequence: UInt32
    public let journalCount: UInt32
    public let items: [UInt32: ScheduledItem]
    public let retiredRecords: Int
    public init(bytes: Data) throws {
        let bytes = Data(bytes)
        guard bytes.count >= 1024 else { throw LegacyItemStoreError.invalidHeader }
        let a = Self.header(Data(bytes[0..<80])), b = Self.header(Data(bytes[512..<592]))
        guard a != nil || b != nil else { throw LegacyItemStoreError.invalidHeader }
        if let a, let b, a.sequence == b.sequence, a.bytes != b.bytes { throw LegacyItemStoreError.ambiguousHeader }
        let header: Header
        if let b, a == nil || b.sequence > a!.sequence { header = b } else { header = a! }
        guard header.flags & 1 == 0 else { throw LegacyItemStoreError.recoveryRequired }
        guard Int(header.count) <= (bytes.count - 1024) / 16 else { throw LegacyItemStoreError.truncatedRecords }
        var items: [UInt32: ScheduledItem] = [:]; items.reserveCapacity(Int(header.count))
        var retired = 0
        for index in 0..<Int(header.count) {
            let offset = 1024 + index * 16, packed = Data(bytes[offset..<offset + 16])
            var reader = ByteReader(packed); let uid = UInt32(try reader.number(4))
            if uid == UInt32.max {
                // The reader writes a canonical fresh dead record when retiring a damaged slot.
                guard packed[4..<16].allSatisfy({ $0 == 0 }) else { throw LegacyItemStoreError.invalidRecord(index) }
                retired += 1; continue
            }
            let item: ScheduledItem
            do { item = try ScheduledItem(decoding: packed) }
            catch { throw LegacyItemStoreError.invalidRecord(index) }
            guard items.updateValue(item, forKey: uid) == nil else { throw LegacyItemStoreError.duplicateUID(uid) }
        }
        sequence = header.sequence; journalCount = header.journalCount
        self.items = items; retiredRecords = retired
    }
    private struct Header {
        let bytes: Data
        let sequence: UInt32
        let count: UInt32
        let journalCount: UInt32
        let flags: UInt16
    }
    private static func header(_ bytes: Data) -> Header? {
        do {
            var reader = ByteReader(bytes)
            guard try reader.take(4) == Data("TIS1".utf8), try reader.number(2) == 1,
                  try reader.number(2) == 80 else { return nil }
            let sequence = UInt32(try reader.number(4)), count = UInt32(try reader.number(4))
            let journal = UInt32(try reader.number(4))
            _ = try reader.take(6); let flags = UInt16(try reader.number(2))
            guard count <= 0x7fff, flags & ~3 == 0, bytes[74] == 0, bytes[75] == 0 else { return nil }
            if flags & 1 != 0 {
                guard try reader.number(4) < UInt64(count) else { return nil }
                _ = try ScheduledItem(decoding: reader.take(16))
            } else { _ = try reader.take(20) }
            if flags & 2 != 0 {
                guard try reader.number(4) < UInt64(count) else { return nil }
                _ = try ScheduledItem(decoding: reader.take(16))
            }
            var checksum = ByteReader(Data(bytes[76..<80]))
            guard try checksum.number(4) == UInt64(legacyCRC32(Data(bytes[0..<76]))) else { return nil }
            return Header(bytes: bytes, sequence: sequence, count: count, journalCount: journal, flags: flags)
        } catch { return nil }
    }
}
func legacyCRC32(_ bytes: Data) -> UInt32 {
    var crc = UInt32.max
    for byte in bytes {
        crc ^= UInt32(byte)
        for _ in 0..<8 { crc = (crc >> 1) ^ (crc & 1 == 0 ? 0 : 0xedb88320) }
    }
    return ~crc
}

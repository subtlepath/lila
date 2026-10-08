import Foundation

public struct VectorFontMetadata: Equatable, Sendable {
    public let faces: Int
}

public enum VectorFontInspector {
    private struct Table { let offset: UInt64; let length: UInt64 }
    public static func inspect(_ url: URL) throws -> VectorFontMetadata {
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        let length = try file.seekToEnd()
        let header = try read(file, 0, 12)
        var offsets: [UInt64] = [0]
        if number(header, 0, 4) == 0x74746366 {
            guard [0x00010000, 0x00020000].contains(number(header, 4, 4)) else { throw ImportError.unsupportedEntry }
            let count = number(header, 8, 4)
            guard count > 0, count <= 256, 12 + count * 4 <= length else { throw ImportError.resourceLimit }
            let records = try read(file, 12, Int(count * 4))
            offsets = (0 ..< Int(count)).map { number(records, $0 * 4, 4) }
            guard Set(offsets).count == offsets.count else { throw ImportError.integrity }
        }
        for offset in offsets { try validateFace(file, offset: offset, length: length) }
        return VectorFontMetadata(faces: offsets.count)
    }
    private static func validateFace(_ file: FileHandle, offset: UInt64, length: UInt64) throws {
        try Task.checkCancellation()
        guard offset % 4 == 0, offset <= length, length - offset >= 12 else { throw ImportError.integrity }
        let header = try read(file, offset, 12)
        let version = number(header, 0, 4)
        guard [0x00010000, 0x4f54544f, 0x74727565].contains(version) else { throw ImportError.unsupportedEntry }
        let count = number(header, 4, 2)
        guard count > 0, count <= 4096, count * 16 <= length - offset - 12 else { throw ImportError.resourceLimit }
        let records = try read(file, offset + 12, Int(count * 16))
        var tables: [String: Table] = [:]
        tables.reserveCapacity(Int(count))
        var previous: String?
        for index in 0 ..< Int(count) {
            let at = index * 16
            let tagBytes = records[at ..< at + 4]
            guard tagBytes.allSatisfy({ (32 ... 126).contains($0) }) else { throw ImportError.integrity }
            let tag = String(decoding: tagBytes, as: UTF8.self)
            guard previous.map({ tag > $0 }) ?? true else { throw ImportError.integrity }
            previous = tag
            let start = number(records, at + 8, 4), size = number(records, at + 12, 4)
            guard start % 4 == 0, start <= length, size <= length - start else { throw ImportError.integrity }
            tables[tag] = Table(offset: start, length: size)
            var remaining = size, consumed: UInt64 = 0, checksum: UInt32 = 0
            try file.seek(toOffset: start)
            while remaining > 0 {
                try Task.checkCancellation()
                let data = try file.read(upToCount: Int(min(remaining, 64 * 1024))) ?? Data()
                guard !data.isEmpty else { throw ImportError.integrity }
                for word in stride(from: 0, to: data.count, by: 4) {
                    var value: UInt32 = 0
                    for byte in 0 ..< 4 {
                        let position = word + byte
                        let zero = tag == "head" && (8 ..< 12).contains(consumed + UInt64(position))
                        value = (value << 8) | (position < data.count && !zero ? UInt32(data[position]) : 0)
                    }
                    checksum &+= value
                }
                consumed += UInt64(data.count); remaining -= UInt64(data.count)
            }
            guard checksum == UInt32(number(records, at + 4, 4)) else { throw ImportError.integrity }
        }
        for (tag, minimum): (String, UInt64) in [("head", 54), ("hhea", 36), ("maxp", 6), ("hmtx", 4), ("cmap", 4), ("name", 6)] {
            guard let table = tables[tag], table.length >= minimum else { throw ImportError.missingEntry }
        }
        if version == 0x4f54544f {
            guard tables["CFF "] != nil || tables["CFF2"] != nil else { throw ImportError.missingEntry }
        } else {
            guard tables["glyf"] != nil, tables["loca"] != nil else { throw ImportError.missingEntry }
        }
    }
    private static func read(_ file: FileHandle, _ offset: UInt64, _ count: Int) throws -> Data {
        try file.seek(toOffset: offset)
        let data = try file.read(upToCount: count) ?? Data()
        guard data.count == count else { throw ImportError.integrity }
        return data
    }
    private static func number(_ data: Data, _ at: Int, _ width: Int) -> UInt64 {
        (0 ..< width).reduce(0) { ($0 << 8) | UInt64(data[at + $1]) }
    }
}

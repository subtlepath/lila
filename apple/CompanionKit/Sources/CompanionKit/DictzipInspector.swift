import Foundation

public struct DictzipMetadata: Equatable, Sendable {
    public let expandedBytes: UInt64
    public let chunkLength: UInt16
    public let chunks: Int
}

public enum DictzipInspector {
    public static func inspect(_ url: URL) throws -> DictzipMetadata {
        let expanded = try GzipInspector.inspect(url)
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        let length = try file.seekToEnd()
        let header = try read(file, offset: 0, count: 12)
        guard header[0] == 31, header[1] == 139, header[2] == 8,
              header[3] & 4 != 0, header[3] & 0xe0 == 0 else { throw ImportError.integrity }
        let extraLength = number(header, 10)
        let extra = try read(file, offset: 12, count: extraLength)
        var at = 0, chunkLength = 0
        var sizes: [Int] = []
        while at < extra.count {
            guard extra.count - at >= 4 else { throw ImportError.integrity }
            let size = number(extra, at + 2)
            guard size <= extra.count - at - 4 else { throw ImportError.integrity }
            if extra[at] == 82 && extra[at + 1] == 65 {
                guard sizes.isEmpty, size >= 6, number(extra, at + 4) == 1 else { throw ImportError.integrity }
                chunkLength = number(extra, at + 6)
                let count = number(extra, at + 8)
                guard chunkLength > 0, count > 0, count <= 8192, size == 6 + count * 2 else { throw ImportError.integrity }
                sizes.reserveCapacity(count)
                for index in 0 ..< count {
                    let compressed = number(extra, at + 10 + index * 2)
                    guard compressed > 4 else { throw ImportError.integrity }
                    sizes.append(compressed)
                }
            }
            at += 4 + size
        }
        guard !sizes.isEmpty, expanded > 0,
              (expanded - 1) / UInt64(chunkLength) + 1 == UInt64(sizes.count) else { throw ImportError.integrity }
        var dataOffset = UInt64(12 + extraLength)
        for mask: UInt8 in [8, 16] where header[3] & mask != 0 {
            // Bound optional filename/comment fields independently of dictionary size.
            var terminated = false
            for _ in 0 ..< 65_536 {
                let byte = try read(file, offset: dataOffset, count: 1)[0]
                dataOffset += 1
                if byte == 0 { terminated = true; break }
            }
            guard terminated else { throw ImportError.resourceLimit }
        }
        if header[3] & 2 != 0 { dataOffset += 2 }
        guard length >= 8, dataOffset <= length - 8 else { throw ImportError.integrity }
        for (index, size) in sizes.enumerated() {
            try Task.checkCancellation()
            guard UInt64(size) <= length - 8 - dataOffset else { throw ImportError.integrity }
            let compressed = try read(file, offset: dataOffset, count: size)
            guard compressed.suffix(4) == Data([0, 0, 255, 255]) else { throw ImportError.integrity }
            let expected = min(UInt64(chunkLength), expanded - UInt64(index) * UInt64(chunkLength))
            let validator = try GzipValidator(limit: expected, windowBits: -15)
            try validator.consume(compressed)
            try validator.finishFlushedChunk(expected: expected)
            dataOffset += UInt64(size)
        }
        // The final DEFLATE terminator follows the independently flushed chunks.
        let terminator = try GzipValidator(limit: 0, windowBits: -15)
        while dataOffset < length - 8 {
            let count = Int(min(64 * 1024, length - 8 - dataOffset))
            try terminator.consume(read(file, offset: dataOffset, count: count))
            dataOffset += UInt64(count)
        }
        guard try terminator.finish() == 0 else { throw ImportError.integrity }
        return DictzipMetadata(expandedBytes: expanded, chunkLength: UInt16(chunkLength), chunks: sizes.count)
    }
    private static func read(_ file: FileHandle, offset: UInt64, count: Int) throws -> Data {
        try file.seek(toOffset: offset)
        let data = try file.read(upToCount: count) ?? Data()
        guard data.count == count else { throw ImportError.integrity }
        return data
    }
    private static func number(_ data: Data, _ at: Int) -> Int { Int(data[at]) | Int(data[at + 1]) << 8 }
}

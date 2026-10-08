import Foundation

public struct BitmapFontMetadata: Equatable, Sendable {
    public let version: UInt16
    public let styles: [UInt8]
}

public enum BitmapFontInspector {
    public static func inspect(_ url: URL) throws -> BitmapFontMetadata {
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        let length = try file.seekToEnd()
        let header = try read(file, 0, 32)
        guard header.prefix(8) == Data([67, 80, 70, 79, 78, 84, 0, 0]) else { throw ImportError.integrity }
        guard number(header, 8, 2) == 4 else { throw ImportError.unsupportedEntry }
        guard number(header, 10, 2) & ~UInt64(1) == 0 else { throw ImportError.unsupportedEntry }
        let bits: UInt64 = header[10] & 1 == 0 ? 1 : 2
        let count = Int(header[12])
        guard (1 ... 4).contains(count) else { throw ImportError.integrity }
        let tocEnd = UInt64(32 + count * 32)
        var styles: [UInt8] = []
        styles.reserveCapacity(count)
        for index in 0 ..< count {
            try Task.checkCancellation()
            let toc = try read(file, UInt64(32 + index * 32), 32)
            guard toc[0] < 4, !styles.contains(toc[0]) else { throw ImportError.integrity }
            styles.append(toc[0])
            let intervals = number(toc, 4, 4), glyphs = number(toc, 8, 4)
            let left = number(toc, 17, 2), right = number(toc, 19, 2)
            let start = number(toc, 24, 4)
            guard intervals <= 4096, glyphs <= 65_536, left <= 4096, right <= 4096,
                  start >= tocEnd else { throw ImportError.integrity }
            let glyphStart = start + intervals * 12
            let bitmapStart = glyphStart + glyphs * 16 + (left + right) * 3 +
                UInt64(toc[21]) * UInt64(toc[22]) + UInt64(toc[23]) * 8
            guard bitmapStart <= length else { throw ImportError.integrity }
            var expected: UInt64 = 0
            var last: UInt64?
            var coverage: [ClosedRange<UInt64>] = []
            coverage.reserveCapacity(Int(intervals))
            for interval in 0 ..< intervals {
                let record = try read(file, start + interval * 12, 12)
                let first = number(record, 0, 4), end = number(record, 4, 4)
                guard first <= end, end <= 0x10ffff, last.map({ first > $0 }) ?? true,
                      number(record, 8, 4) == expected,
                      expected <= glyphs, end - first + 1 <= glyphs - expected else { throw ImportError.integrity }
                expected += end - first + 1; last = end
                coverage.append(first ... end)
            }
            guard expected == glyphs else { throw ImportError.integrity }
            for glyph in 0 ..< glyphs {
                try Task.checkCancellation()
                let record = try read(file, glyphStart + glyph * 16, 16)
                let size = number(record, 8, 2), offset = number(record, 12, 4)
                let required = (UInt64(record[0]) * UInt64(record[1]) * bits + 7) / 8
                guard size == required, offset <= length - bitmapStart,
                      size <= length - bitmapStart - offset else { throw ImportError.integrity }
            }
            let leftStart = glyphStart + glyphs * 16
            for (start, entries, classes) in [(leftStart, left, toc[21]), (leftStart + left * 3, right, toc[22])] {
                var previous: UInt64?
                for entry in 0 ..< entries {
                    let record = try read(file, start + entry * 3, 3)
                    let cp = number(record, 0, 2)
                    guard previous.map({ cp > $0 }) ?? true, record[2] > 0, record[2] <= classes,
                          covers(cp, coverage) else { throw ImportError.integrity }
                    previous = cp
                }
            }
            let ligatureStart = bitmapStart - UInt64(toc[23]) * 8
            var previousPair: UInt64?
            for entry in 0 ..< UInt64(toc[23]) {
                let record = try read(file, ligatureStart + entry * 8, 8)
                let pair = number(record, 0, 4), replacement = number(record, 4, 4)
                guard previousPair.map({ pair > $0 }) ?? true, covers(pair >> 16, coverage),
                      covers(pair & 0xffff, coverage), covers(replacement, coverage) else { throw ImportError.integrity }
                previousPair = pair
            }
        }
        return BitmapFontMetadata(version: 4, styles: styles)
    }
    private static func covers(_ codepoint: UInt64, _ intervals: [ClosedRange<UInt64>]) -> Bool {
        var start = 0, end = intervals.count
        while start < end {
            let middle = start + (end - start) / 2
            if intervals[middle].upperBound < codepoint { start = middle + 1 } else { end = middle }
        }
        return start < intervals.count && intervals[start].contains(codepoint)
    }
    private static func read(_ file: FileHandle, _ offset: UInt64, _ count: Int) throws -> Data {
        try file.seek(toOffset: offset)
        let data = try file.read(upToCount: count) ?? Data()
        guard data.count == count else { throw ImportError.integrity }
        return data
    }
    private static func number(_ data: Data, _ at: Int, _ width: Int) -> UInt64 {
        (0 ..< width).reduce(0) { $0 | UInt64(data[at + $1]) << (8 * $1) }
    }
}

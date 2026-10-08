import Foundation
import ZIPFoundation

public enum ImportError: Error, Equatable, Sendable {
    case unsafePath, duplicatePath, unsupportedEntry, resourceLimit, integrity, missingEntry
}

public struct ArchiveLimits: Sendable {
    public var entries: Int
    public var entryBytes: UInt64
    public var totalBytes: UInt64
    public init(entries: Int = 20_000, entryBytes: UInt64 = 256 * 1024 * 1024,
                totalBytes: UInt64 = 1024 * 1024 * 1024) {
        self.entries = entries; self.entryBytes = entryBytes; self.totalBytes = totalBytes
    }
}

public struct ValidatedArchive: Sendable {
    public let paths: Set<String>
    public let uncompressedBytes: UInt64
}

public enum ArchiveValidator {
    // Validation streams each file and never writes archive-controlled paths to disk.
    public static func validate(_ url: URL, limits: ArchiveLimits = ArchiveLimits()) throws -> ValidatedArchive {
        try validate(url, limits: limits, visit: nil)
    }
    static func entries(_ url: URL) throws -> [String: Entry] {
        var entries: [String: Entry] = [:]
        entries.reserveCapacity(1024)
        _ = try validate(url, limits: ArchiveLimits()) { path, entry in entries[path] = entry }
        return entries
    }
    private static func validate(_ url: URL, limits: ArchiveLimits,
                                 visit: ((String, Entry) -> Void)?) throws -> ValidatedArchive {
        guard limits.entries > 0 else { throw ImportError.resourceLimit }
        let layout = try directoryLayout(url)
        let expectedEntries = layout.count
        let metadata = try FileHandle(forReadingFrom: url)
        defer { try? metadata.close() }
        var central = layout.offset
        guard expectedEntries <= UInt64(limits.entries) else { throw ImportError.resourceLimit }
        let archive = try Archive(url: url, accessMode: .read)
        var paths = Set<String>()
        paths.reserveCapacity(min(limits.entries, 1024))
        var total: UInt64 = 0
        for entry in archive {
            try Task.checkCancellation()
            guard paths.count < limits.entries else { throw ImportError.resourceLimit }
            guard entry.type != .symlink else { throw ImportError.unsupportedEntry }
            guard central <= layout.end, layout.end - central >= 46 else { throw ImportError.integrity }
            try metadata.seek(toOffset: central)
            let header = try metadata.read(upToCount: 46) ?? Data()
            guard header.count == 46, integer(header, 0, 4) == 0x02014b50 else { throw ImportError.integrity }
            let nameBytes = integer(header, 28, 2)
            let variableBytes = nameBytes + integer(header, 30, 2) + integer(header, 32, 2)
            guard nameBytes > 0, nameBytes <= 1024, variableBytes <= layout.end - central - 46 else {
                throw ImportError.integrity
            }
            let rawName = try metadata.read(upToCount: Int(nameBytes)) ?? Data()
            guard rawName.count == Int(nameBytes) else { throw ImportError.integrity }
            let name = try ZipHeaderName.decode(rawName, utf8: integer(header, 8, 2) & 0x800 != 0)
            let path = try canonicalPath(name, directory: entry.type == .directory)
            central += 46 + variableBytes
            guard paths.insert(path).inserted else { throw ImportError.duplicatePath }
            visit?(path, entry)
            guard entry.uncompressedSize <= limits.entryBytes,
                  total <= limits.totalBytes,
                  entry.uncompressedSize <= limits.totalBytes - total else { throw ImportError.resourceLimit }
            if entry.type == .directory {
                guard entry.uncompressedSize == 0 else { throw ImportError.integrity }
                continue
            }
            var consumed: UInt64 = 0
            let checksum = try archive.extract(entry, bufferSize: 64 * 1024) { bytes in
                try Task.checkCancellation()
                guard consumed <= entry.uncompressedSize,
                      UInt64(bytes.count) <= entry.uncompressedSize - consumed else { throw ImportError.integrity }
                consumed += UInt64(bytes.count)
            }
            guard consumed == entry.uncompressedSize, checksum == entry.checksum else { throw ImportError.integrity }
            total += consumed
        }
        guard UInt64(paths.count) == expectedEntries else { throw ImportError.integrity }
        if central != layout.end {
            guard central < layout.end, layout.end - central >= 6 else { throw ImportError.integrity }
            try metadata.seek(toOffset: central)
            let signature = try metadata.read(upToCount: 6) ?? Data()
            guard signature.count == 6, integer(signature, 0, 4) == 0x05054b50,
                  integer(signature, 4, 2) == layout.end - central - 6 else { throw ImportError.integrity }
        }
        guard try metadata.seekToEnd() == layout.length else { throw ImportError.integrity }
        guard !paths.isEmpty else { throw ImportError.missingEntry }
        return ValidatedArchive(paths: paths, uncompressedBytes: total)
    }

    private struct DirectoryLayout {
        let count: UInt64
        let offset: UInt64
        let end: UInt64
        let length: UInt64
    }
    private static func directoryLayout(_ url: URL) throws -> DirectoryLayout {
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        let length = try file.seekToEnd()
        guard length >= 22 else { throw ImportError.integrity }
        let tailSize = min(length, 65_557)
        try file.seek(toOffset: length - tailSize)
        let tail = try file.read(upToCount: Int(tailSize)) ?? Data()
        guard tail.count == Int(tailSize) else { throw ImportError.integrity }
        for at in stride(from: tail.count - 22, through: 0, by: -1) {
            guard integer(tail, at, 4) == 0x06054b50,
                  at + 22 + Int(integer(tail, at + 20, 2)) == tail.count else { continue }
            guard integer(tail, at + 4, 2) == 0, integer(tail, at + 6, 2) == 0 else {
                throw ImportError.unsupportedEntry
            }
            let count32 = integer(tail, at + 10, 2), diskCount = integer(tail, at + 8, 2)
            let bytes32 = integer(tail, at + 12, 4), offset32 = integer(tail, at + 16, 4)
            var count = count32, bytes = bytes32, offset = offset32
            let endOffset = length - tailSize + UInt64(at)
            var records = endOffset
            let required = count32 == 0xffff || diskCount == 0xffff || bytes32 == 0xffffffff || offset32 == 0xffffffff
            var locator = Data()
            if endOffset >= 20 {
                try file.seek(toOffset: endOffset - 20)
                locator = try file.read(upToCount: 20) ?? Data()
                guard locator.count == 20 else { throw ImportError.integrity }
            }
            if locator.count == 20 && integer(locator, 0, 4) == 0x07064b50 {
                guard integer(locator, 4, 4) == 0, integer(locator, 16, 4) == 1 else { throw ImportError.integrity }
                records = integer(locator, 8, 8)
                guard records <= endOffset - 20, endOffset - 20 - records >= 56 else { throw ImportError.integrity }
                try file.seek(toOffset: records)
                let record = try file.read(upToCount: 56) ?? Data()
                guard record.count == 56, integer(record, 0, 4) == 0x06064b50,
                      integer(record, 4, 8) >= 44, integer(record, 4, 8) == endOffset - 20 - records - 12,
                      integer(record, 14, 2) >= 45, integer(record, 16, 4) == 0, integer(record, 20, 4) == 0,
                      integer(record, 24, 8) == integer(record, 32, 8) else { throw ImportError.integrity }
                count = integer(record, 32, 8); bytes = integer(record, 40, 8); offset = integer(record, 48, 8)
                guard (count32 == 0xffff || count32 == count), (diskCount == 0xffff || diskCount == count),
                      (bytes32 == 0xffffffff || bytes32 == bytes), (offset32 == 0xffffffff || offset32 == offset) else {
                    throw ImportError.integrity
                }
            } else {
                guard !required, count32 == diskCount else { throw ImportError.integrity }
            }
            guard offset <= records, bytes <= records - offset, count <= bytes / 46 else { throw ImportError.integrity }
            return DirectoryLayout(count: count, offset: offset, end: offset + bytes, length: length)
        }
        throw ImportError.integrity
    }

    private static func integer(_ data: Data, _ offset: Int, _ count: Int) -> UInt64 {
        var value: UInt64 = 0
        for index in 0 ..< count { value |= UInt64(data[offset + index]) << (index * 8) }
        return value
    }

    static func canonicalPath(_ value: String, directory: Bool) throws -> String {
        guard !value.isEmpty, value.utf8.count <= 1024, !value.hasPrefix("/"),
              !value.contains("\\"), !value.contains(":"),
              !value.unicodeScalars.contains(where: { $0.value < 32 || $0.value == 127 }) else {
            throw ImportError.unsafePath
        }
        let path = directory && value.hasSuffix("/") ? String(value.dropLast()) : value
        let components = path.split(separator: "/", omittingEmptySubsequences: false)
        guard components.allSatisfy({ !$0.isEmpty && $0 != "." && $0 != ".." }) else { throw ImportError.unsafePath }
        return path.precomposedStringWithCanonicalMapping
    }
}

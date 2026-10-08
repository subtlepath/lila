import Foundation
import CCompanionZlib

public enum GzipInspector {
    public static func inspect(_ url: URL, expandedLimit: UInt64 = 1024 * 1024 * 1024) throws -> UInt64 {
        let validator = try GzipValidator(limit: expandedLimit)
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        while let data = try file.read(upToCount: 64 * 1024), !data.isEmpty {
            try validator.consume(data)
        }
        return try validator.finish()
    }
}

// Inflation discards output through one reusable 64 KiB buffer; zlib checks the gzip trailer.
final class GzipValidator {
    private static let bufferSize = 64 * 1024
    private var stream = z_stream()
    private var output = [UInt8](repeating: 0, count: bufferSize)
    private let limit: UInt64
    private var expanded: UInt64 = 0
    private var ended = false
    init(limit: UInt64, windowBits: Int32 = 31) throws {
        self.limit = limit
        guard inflateInit2_(&stream, windowBits, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
            throw ImportError.resourceLimit
        }
    }
    deinit { inflateEnd(&stream) }
    func consume(_ data: Data) throws {
        try Task.checkCancellation()
        if data.isEmpty { return }
        guard !ended, data.count <= Int(UInt32.max) else { throw ImportError.integrity }
        try data.withUnsafeBytes { bytes in
            stream.next_in = UnsafeMutablePointer(mutating: bytes.bindMemory(to: UInt8.self).baseAddress)
            stream.avail_in = UInt32(data.count)
            defer { stream.next_in = nil; stream.avail_in = 0 }
            var drain = true
            while stream.avail_in > 0 || drain {
                try Task.checkCancellation()
                let available = stream.avail_in
                let status = output.withUnsafeMutableBufferPointer { buffer in
                    stream.next_out = buffer.baseAddress
                    stream.avail_out = UInt32(Self.bufferSize)
                    defer { stream.next_out = nil }
                    return inflate(&stream, Z_NO_FLUSH)
                }
                let produced = UInt64(Self.bufferSize) - UInt64(stream.avail_out)
                guard expanded <= limit, produced <= limit - expanded else { throw ImportError.resourceLimit }
                expanded += produced
                if status == Z_STREAM_END {
                    guard stream.avail_in == 0 else { throw ImportError.integrity }
                    ended = true; break
                }
                guard status == Z_OK || (status == Z_BUF_ERROR && stream.avail_in == 0 && produced == 0) else {
                    throw ImportError.integrity
                }
                drain = stream.avail_out == 0
                if produced == 0 && stream.avail_in == available {
                    guard stream.avail_in == 0 else { throw ImportError.integrity }
                    break
                }
            }
        }
    }
    func finish() throws -> UInt64 {
        guard ended else { throw ImportError.integrity }
        return expanded
    }
    func finishFlushedChunk(expected: UInt64) throws {
        guard !ended, expanded == expected else { throw ImportError.integrity }
    }
}

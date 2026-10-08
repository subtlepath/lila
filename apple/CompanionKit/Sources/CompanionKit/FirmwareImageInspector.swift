import Foundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif

public enum FirmwareImageError: Error, Equatable { case length, header, chip, board, checksum, hash, releaseHash }
public enum FirmwareImageInspector {
    /// Streams bounded chunks; returned SHA-256 covers the complete image, including its trailer.
    public static func inspect(_ url: URL, compatibility: FirmwareReleaseCompatibility) throws -> Data {
        try Task.checkCancellation()
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        let size = try file.seekToEnd()
        guard size == compatibility.length, size <= compatibility.otaPartitionBytes else { throw FirmwareImageError.length }
        try file.seek(toOffset: 0)
        var position: UInt64 = 0
        var payloadHash = SHA256(), completeHash = SHA256()
        func read(_ count: Int, payload: Bool = true) throws -> Data {
            try Task.checkCancellation()
            guard count >= 0, UInt64(count) <= size - position else { throw FirmwareImageError.length }
            guard let bytes = try file.read(upToCount: count), bytes.count == count else { throw FirmwareImageError.length }
            position += UInt64(count)
            completeHash.update(data: bytes)
            if payload { payloadHash.update(data: bytes) }
            return bytes
        }
        func number(_ bytes: Data, _ at: Int, _ count: Int) -> UInt64 {
            (0 ..< count).reduce(0) { $0 | UInt64(bytes[at + $1]) << (8 * $1) }
        }
        let header = try read(24)
        guard header[0] == 0xe9, (1 ... 16).contains(header[1]), header[23] <= 1 else { throw FirmwareImageError.header }
        guard number(header, 12, 2) == compatibility.chipID else { throw FirmwareImageError.chip }
        let prefix = Array("CROSSPOINT-BOARD-V1:".utf8)
        let board = Array(compatibility.boardTags[0].utf8)
        var checksum: UInt8 = 0xef
        var found = false
        for _ in 0 ..< header[1] {
            let segment = try read(8)
            var remaining = number(segment, 4, 4)
            guard remaining <= size - position else { throw FirmwareImageError.length }
            var prefixAt = 0, tag: [UInt8] = []
            tag.reserveCapacity(24)
            var inTag = false
            while remaining > 0 {
                let bytes = try read(Int(min(remaining, 16384)))
                remaining -= UInt64(bytes.count)
                for byte in bytes {
                    checksum ^= byte
                    if inTag {
                        if byte == 59 {
                            guard tag == board else { throw FirmwareImageError.board }
                            found = true; inTag = false; prefixAt = 0; tag.removeAll(keepingCapacity: true)
                        } else {
                            guard byte < 128, tag.count < 23 else { throw FirmwareImageError.board }
                            tag.append(byte)
                        }
                    } else if byte == prefix[prefixAt] {
                        prefixAt += 1
                        if prefixAt == prefix.count { inTag = true; prefixAt = 0 }
                    } else { prefixAt = byte == prefix[0] ? 1 : 0 }
                }
            }
            if inTag { throw FirmwareImageError.board }
        }
        guard found else { throw FirmwareImageError.board }
        let padded = (position + 16) & ~UInt64(15)
        guard padded + (header[23] == 1 ? 32 : 0) == size else { throw FirmwareImageError.length }
        let padding = try read(Int(padded - position))
        guard padding.last == checksum else { throw FirmwareImageError.checksum }
        if header[23] == 1 {
            let expected = Data(payloadHash.finalize())
            guard try read(32, payload: false) == expected else { throw FirmwareImageError.hash }
        }
        guard try file.seekToEnd() == size else { throw FirmwareImageError.length }
        try Task.checkCancellation()
        let digest = Data(completeHash.finalize())
        guard digest == compatibility.sha256 else { throw FirmwareImageError.releaseHash }
        return digest
    }
}

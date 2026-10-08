import Foundation
import ZIPFoundation
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

public struct DictionaryBundleMetadata: Sendable {
    public let info: DictionaryInfo
    public let basePath: String
    public let members: [String]
}

public enum DictionaryBundleInspector {
    public static func inspect(_ url: URL, scratchDirectory: URL = FileManager.default.temporaryDirectory) throws -> DictionaryBundleMetadata {
        let entries = try ArchiveValidator.entries(url)
        let archive = try Archive(url: url, accessMode: .read)
        let headers = entries.keys.filter { $0.hasSuffix(".ifo") }
        guard headers.count == 1, let path = headers.first else { throw ImportError.integrity }
        guard let header = entries[path], header.type == .file else { throw ImportError.missingEntry }
        guard header.uncompressedSize <= 64 * 1024 else { throw ImportError.resourceLimit }
        var bytes = Data()
        bytes.reserveCapacity(Int(header.uncompressedSize))
        try extract(header, archive: archive) { bytes.append($0) }
        let info = try DictionaryInfo(data: bytes)
        let base = String(path.dropLast(4))
        guard let index = entries[base + ".idx"], index.type == .file else { throw ImportError.missingEntry }
        let definitionPath: String
        let definitionBytes: UInt64
        if let definitions = entries[base + ".dict"], definitions.type == .file {
            definitionPath = base + ".dict"
            definitionBytes = definitions.uncompressedSize
        } else {
            guard let compressed = entries[base + ".dict.dz"], compressed.type == .file else { throw ImportError.missingEntry }
            definitionPath = base + ".dict.dz"
            definitionBytes = try inspectCompressed(compressed, archive: archive, directory: scratchDirectory)
        }
        guard index.uncompressedSize == UInt64(info.indexBytes) else { throw ImportError.integrity }
        var indexValidator = DictionaryIndexValidator(words: info.wordCount, bytes: index.uncompressedSize,
                                                      mode: .definitions(length: definitionBytes))
        try extract(index, archive: archive) { try indexValidator.consume($0) }
        try indexValidator.finish()
        var members = [path, base + ".idx", definitionPath]
        if let synonyms = entries[base + ".syn"] {
            guard synonyms.type == .file, let count = info.synonymCount else { throw ImportError.integrity }
            var synonymValidator = DictionaryIndexValidator(words: count, bytes: synonyms.uncompressedSize,
                                                            mode: .synonyms(words: info.wordCount))
            try extract(synonyms, archive: archive) { try synonymValidator.consume($0) }
            try synonymValidator.finish()
            members.append(base + ".syn")
        } else if let count = info.synonymCount, count != 0 { throw ImportError.missingEntry }
        return DictionaryBundleMetadata(info: info, basePath: base, members: members)
    }
    private static func inspectCompressed(_ entry: Entry, archive: Archive, directory: URL) throws -> UInt64 {
        let (temporary, output) = try createExclusiveStage(in: directory)
        defer { try? output.close(); try? FileManager.default.removeItem(at: temporary) }
        try extract(entry, archive: archive) { try output.write(contentsOf: $0) }
        try output.close()
        return try DictzipInspector.inspect(temporary).expandedBytes
    }
    private static func extract(_ entry: Entry, archive: Archive, consumer: (Data) throws -> Void) throws {
        var count: UInt64 = 0
        let checksum = try archive.extract(entry, bufferSize: 64 * 1024) { bytes in
            try Task.checkCancellation()
            guard count <= entry.uncompressedSize, UInt64(bytes.count) <= entry.uncompressedSize - count else {
                throw ImportError.integrity
            }
            count += UInt64(bytes.count)
            try consumer(bytes)
        }
        guard count == entry.uncompressedSize, checksum == entry.checksum else { throw ImportError.integrity }
    }
}

// UUID names are owned by vault recovery; O_EXCL also protects against collisions.
func createExclusiveStage(in directory: URL) throws -> (URL, FileHandle) {
    guard directory.isFileURL else { throw VaultError.invalidSource }
    let url = directory.appendingPathComponent(UUID().uuidString + ".partial")
    let descriptor = url.withUnsafeFileSystemRepresentation { path in
        guard let path else { return Int32(-1) }
        return open(path, O_WRONLY | O_CREAT | O_EXCL, mode_t(0o600))
    }
    guard descriptor >= 0 else { throw VaultError.io(errno) }
    return (url, FileHandle(fileDescriptor: descriptor, closeOnDealloc: true))
}

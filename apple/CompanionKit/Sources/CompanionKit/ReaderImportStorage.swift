import Foundation
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

public enum ReaderImportStorageError: Error, Equatable, Sendable {
    case binding, phase, offset, length, unsafePath
}

// Files are synchronized before the caller advances SQLite's acknowledged offset.
public actor ReaderImportStorage {
    private let root: URL
    public init(root: URL) throws {
        guard root.isFileURL else { throw ReaderImportStorageError.unsafePath }
        self.root = root
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        try Self.requireDirectory(root)
        try Self.synchronizeDirectory(root)
        try Self.synchronizeDirectory(root.deletingLastPathComponent())
    }
    @discardableResult
    public func prepare(_ job: ReaderImportJob) throws -> UInt64 {
        let lock = try VaultLock(root: root, exclusive: true, nonblocking: false)
        defer { lock.release() }
        return try prepareLocked(job)
    }
    public func append(_ bytes: Data, to job: ReaderImportJob) throws -> UInt64 {
        guard job.phase == .downloading else { throw ReaderImportStorageError.phase }
        guard !bytes.isEmpty, bytes.count <= ReaderContentReadRequest.maximumChunkBytes,
              job.acknowledgedOffset <= job.manifest.length,
              UInt64(bytes.count) <= job.manifest.length - job.acknowledgedOffset else {
            throw ReaderImportStorageError.length
        }
        let lock = try VaultLock(root: root, exclusive: true, nonblocking: false)
        defer { lock.release() }
        _ = try prepareLocked(job)
        let file = try openFile(path(job).appendingPathComponent("download"), create: false)
        defer { try? file.close() }
        try file.seek(toOffset: job.acknowledgedOffset)
        try file.write(contentsOf: bytes)
        try file.synchronize()
        return job.acknowledgedOffset + UInt64(bytes.count)
    }
    public func completedSource(_ job: ReaderImportJob) throws -> URL {
        guard job.phase == .verifying, job.acknowledgedOffset == job.manifest.length else {
            throw ReaderImportStorageError.phase
        }
        let lock = try VaultLock(root: root, exclusive: true, nonblocking: false)
        defer { lock.release() }
        _ = try prepareLocked(job)
        return path(job).appendingPathComponent("download")
    }
    public func discard(_ job: ReaderImportJob) throws {
        guard job.phase == .completed || job.phase == .aborted else { throw ReaderImportStorageError.phase }
        let lock = try VaultLock(root: root, exclusive: true, nonblocking: false)
        defer { lock.release() }
        let directory = path(job)
        guard FileManager.default.fileExists(atPath: directory.path) else { return }
        try Self.requireDirectory(directory)
        let entries = try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)
        if !entries.isEmpty {
            try verifyBinding(job, directory: directory)
            for entry in entries where entry.lastPathComponent != "binding" {
                try FileManager.default.removeItem(at: entry)
            }
            // Preserve the ownership proof until payload deletion is durable.
            try Self.synchronizeDirectory(directory)
            try FileManager.default.removeItem(at: directory.appendingPathComponent("binding"))
            try Self.synchronizeDirectory(directory)
        }
        let result = directory.withUnsafeFileSystemRepresentation { path in
            guard let path else { return Int32(-1) }
            return rmdir(path)
        }
        guard result == 0 else { throw VaultError.io(errno) }
        try Self.synchronizeDirectory(root)
    }
    private func prepareLocked(_ job: ReaderImportJob) throws -> UInt64 {
        guard [.queued, .downloading, .paused, .verifying].contains(job.phase) else {
            throw ReaderImportStorageError.phase
        }
        guard job.manifest.length <= UInt64(Int64.max), job.acknowledgedOffset <= job.manifest.length,
              (job.phase != .queued || job.acknowledgedOffset == 0),
              (job.phase != .verifying || job.acknowledgedOffset == job.manifest.length) else {
            throw ReaderImportStorageError.offset
        }
        _ = try ReaderContentReadRequest(generation: job.generation, manifest: job.manifest, offset: 0, maximumBytes: 1)
        guard job.reader.count == 16, job.reader.contains(where: { $0 != 0 }),
              job.installation.count == 16, job.installation.contains(where: { $0 != 0 }),
              withUnsafeBytes(of: job.id.uuid, { $0.contains(where: { $0 != 0 }) }) else {
            throw ReaderImportStorageError.binding
        }
        let directory = path(job)
        if !FileManager.default.fileExists(atPath: directory.path) {
            guard job.acknowledgedOffset == 0 else { throw ReaderImportStorageError.offset }
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
            try Self.synchronizeDirectory(root)
        }
        try Self.requireDirectory(directory)
        let bindingURL = directory.appendingPathComponent("binding")
        if !FileManager.default.fileExists(atPath: bindingURL.path) {
            guard job.acknowledgedOffset == 0 else { throw ReaderImportStorageError.offset }
            let (temporary, binding) = try createExclusiveStage(in: directory)
            defer { try? binding.close(); try? FileManager.default.removeItem(at: temporary) }
            try binding.write(contentsOf: bindingBytes(job))
            try binding.synchronize()
            try binding.close()
            let result = temporary.withUnsafeFileSystemRepresentation { from in
                bindingURL.withUnsafeFileSystemRepresentation { to in
                    guard let from, let to else { return Int32(-1) }
                    return link(from, to)
                }
            }
            guard result == 0 else { throw VaultError.io(errno) }
            try Self.synchronizeDirectory(directory)
        }
        try verifyBinding(job, directory: directory)
        let download = directory.appendingPathComponent("download")
        let exists = FileManager.default.fileExists(atPath: download.path)
        guard exists || job.acknowledgedOffset == 0 else { throw ReaderImportStorageError.offset }
        let file = try openFile(download, create: !exists, exclusive: !exists)
        defer { try? file.close() }
        let length = try file.seekToEnd()
        guard length >= job.acknowledgedOffset else { throw ReaderImportStorageError.offset }
        if length != job.acknowledgedOffset { try file.truncate(atOffset: job.acknowledgedOffset) }
        try file.synchronize()
        try Self.synchronizeDirectory(directory)
        return job.acknowledgedOffset
    }
    private func path(_ job: ReaderImportJob) -> URL { root.appendingPathComponent(job.id.uuidString, isDirectory: true) }
    private func bindingBytes(_ job: ReaderImportJob) -> Data {
        var bytes = Data([0x4c, 0x49, 0x42, 1]); bytes.reserveCapacity(115)
        bytes.append(job.reader); bytes.append(job.generation); bytes.append(job.installation); bytes.append(job.manifest.encoded)
        return bytes
    }
    private func verifyBinding(_ job: ReaderImportJob, directory: URL) throws {
        let file = try openFile(directory.appendingPathComponent("binding"), create: false)
        defer { try? file.close() }
        guard try file.read(upToCount: 116) == bindingBytes(job) else { throw ReaderImportStorageError.binding }
    }
    private func openFile(_ url: URL, create: Bool, exclusive: Bool = false) throws -> FileHandle {
        let flags = O_RDWR | O_NOFOLLOW | (create ? O_CREAT : 0) | (exclusive ? O_EXCL : 0)
        let descriptor = url.withUnsafeFileSystemRepresentation { path in
            guard let path else { return Int32(-1) }
            return open(path, flags, mode_t(0o600))
        }
        guard descriptor >= 0 else { throw VaultError.io(errno) }
        var info = stat()
        guard fstat(descriptor, &info) == 0, (info.st_mode & mode_t(S_IFMT)) == mode_t(S_IFREG) else {
            _ = close(descriptor); throw ReaderImportStorageError.unsafePath
        }
        return FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
    }
    private static func requireDirectory(_ url: URL) throws {
        guard try FileManager.default.attributesOfItem(atPath: url.path)[.type] as? FileAttributeType == .typeDirectory else {
            throw ReaderImportStorageError.unsafePath
        }
    }
    private static func synchronizeDirectory(_ url: URL) throws {
        let descriptor = url.withUnsafeFileSystemRepresentation { path in
            guard let path else { return Int32(-1) }
            return open(path, O_RDONLY | O_NOFOLLOW)
        }
        guard descriptor >= 0 else { throw VaultError.io(errno) }
        defer { _ = close(descriptor) }
        guard fsync(descriptor) == 0 else { throw VaultError.io(errno) }
    }
}

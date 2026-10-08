import Foundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

public enum VaultError: Error, Equatable, Sendable { case invalidSource, integrity, io(Int32) }
public struct StoredObject: Equatable, Sendable {
    public let id: ContentID
    public let length: UInt64
    public let url: URL
}

// Content objects are immutable. Publish SQLite metadata only after import returns.
public actor ContentVault {
    private let root: URL
    private let objects: URL
    private let staging: URL
    public init(root: URL) throws {
        self.root = root
        objects = root.appendingPathComponent("objects", isDirectory: true)
        staging = root.appendingPathComponent("staging", isDirectory: true)
        try FileManager.default.createDirectory(at: objects, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
        try syncDirectory(root)
        try syncDirectory(root.deletingLastPathComponent())
        _ = try reclaimStages(root: root, staging: staging)
    }
    public func importFile(_ source: URL) throws -> StoredObject {
        try stage(source, validate: { _ in () }).0
    }
    public func importValidatedFile<Metadata: Sendable>(
        _ source: URL, validate: @Sendable (URL) throws -> Metadata
    ) throws -> (StoredObject, Metadata) {
        try stage(source, validate: validate)
    }
    public func importValidatedFile<Metadata: Sendable>(
        _ source: URL, expectedID: ContentID, expectedLength: UInt64,
        validate: @Sendable (URL) throws -> Metadata
    ) throws -> (StoredObject, Metadata) {
        try stage(source, expectedID: expectedID, expectedLength: expectedLength, validate: validate)
    }
    private func stage<Metadata: Sendable>(
        _ source: URL, expectedID: ContentID? = nil, expectedLength: UInt64? = nil,
        validate: (URL) throws -> Metadata
    ) throws -> (StoredObject, Metadata) {
        try Task.checkCancellation()
        guard source.isFileURL else { throw VaultError.invalidSource }
#if canImport(Darwin)
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
#endif
        guard try FileManager.default.attributesOfItem(atPath: source.path)[.type] as? FileAttributeType == .typeRegular else {
            throw VaultError.invalidSource
        }
        let lock = try VaultLock(root: root, exclusive: false, nonblocking: false)
        defer { lock.release() }
        let (temporary, output) = try createExclusiveStage(in: staging)
        defer { try? output.close(); try? FileManager.default.removeItem(at: temporary) }
        let input = try FileHandle(forReadingFrom: source)
        defer { try? input.close() }
        var hasher = SHA256()
        var length: UInt64 = 0
        while let bytes = try input.read(upToCount: 64 * 1024), !bytes.isEmpty {
            try Task.checkCancellation()
            guard UInt64(bytes.count) <= UInt64(Int64.max) - length else { throw StoreError.invalidValue }
            try output.write(contentsOf: bytes)
            hasher.update(data: bytes)
            length += UInt64(bytes.count)
        }
        try Task.checkCancellation()
        try output.synchronize()
        try output.close()
        let id = try ContentID(hasher.finalize().map { String(format: "%02x", $0) }.joined())
        guard (expectedID == nil || expectedID == id), (expectedLength == nil || expectedLength == length) else {
            throw VaultError.integrity
        }
        let metadata = try validate(temporary)
        try Task.checkCancellation()
        let directory = objects.appendingPathComponent(String(id.hex.prefix(2)), isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try syncDirectory(objects)
        let destination = directory.appendingPathComponent(id.hex)
        // A hard link publishes atomically without replacing an existing object.
        let result = temporary.withUnsafeFileSystemRepresentation { from in
            destination.withUnsafeFileSystemRepresentation { to in
                guard let from, let to else { return Int32(-1) }
                return link(from, to)
            }
        }
        if result != 0 {
            let error = errno
            guard error == EEXIST else { throw VaultError.io(error) }
            guard try digest(destination) == (id, length) else { throw VaultError.integrity }
        }
        try syncDirectory(directory)
        return (StoredObject(id: id, length: length, url: destination), metadata)
    }
    public func reclaimInterruptedImports() throws -> Int {
        try reclaimStages(root: root, staging: staging)
    }
    public func storeTintaDerivedInstallation(_ installation: TintaDerivedInstallation) throws -> StoredObject {
        let lock = try VaultLock(root: root, exclusive: false, nonblocking: false)
        defer { lock.release() }
        for bytes in [installation.files.itemState, installation.files.localReviews,
                      installation.files.completedLessons, installation.files.completedReadings, installation.files.dayLog] {
            _ = try importInstallationBytes(bytes)
        }
        return try importInstallationBytes(installation.manifest)
    }
    private func importInstallationBytes(_ bytes: Data) throws -> StoredObject {
        try Task.checkCancellation()
        let (source, handle) = try createExclusiveStage(in: staging)
        defer { try? handle.close(); try? FileManager.default.removeItem(at: source) }
        try handle.write(contentsOf: bytes)
        try handle.synchronize()
        try handle.close()
        return try stage(source, validate: { _ in () }).0
    }
    public func restoreTintaDerivedInstallation(manifest: ContentID, maximumBytes: UInt64) throws -> TintaDerivedInstallation {
        let lock = try VaultLock(root: root, exclusive: false, nonblocking: false)
        defer { lock.release() }
        let object = try verifiedObject(manifest)
        guard object.length == 332, maximumBytes >= 332 else { throw ProtocolError.length }
        let bytes = try Data(contentsOf: object.url)
        let receipt = try TintaDerivedReceipt(decoding: bytes)
        var total: UInt64 = 332
        for file in receipt.files {
            guard file.length <= maximumBytes - total else { throw ProtocolError.length }
            total += file.length
        }
        var parts: [Data] = []; parts.reserveCapacity(5)
        for file in receipt.files {
            try Task.checkCancellation()
            let id = try ContentID(file.hash.map { String(format: "%02x", $0) }.joined())
            let stored = try verifiedObject(id)
            guard stored.length == file.length else { throw VaultError.integrity }
            parts.append(try Data(contentsOf: stored.url))
        }
        let files = TintaDerivedCourseFiles(course: receipt.course, studyDay: receipt.studyDay,
            itemState: parts[0], localReviews: parts[1], completedLessons: parts[2],
            completedReadings: parts[3], dayLog: parts[4])
        return try TintaDerivedInstallation(files: files, manifest: bytes)
    }
    public func tintaDerivedInstallation(content: ContentID, course: Data, journal: [JournalMutation],
                                         studyDay: UInt16, storageGeneration: Data, snapshotIdentity: Data,
                                         revision: UInt64, expectedLength: UInt64? = nil,
                                         expectedDetails: CoursePackDetails? = nil) throws -> TintaDerivedInstallation {
        let lock = try VaultLock(root: root, exclusive: false, nonblocking: false)
        defer { lock.release() }
        let object = try verifiedObject(content)
        let metadata = try CoursePackInspector.inspect(object.url)
        let inspectedDetails = try CoursePackDetails(metadata)
        guard expectedLength == nil || expectedLength == object.length,
              expectedDetails == nil || expectedDetails == inspectedDetails else {
            throw VaultError.integrity
        }
        let installation = try TintaHistory.derivedInstallation(journal, course: course, metadata: metadata,
            studyDay: studyDay, storageGeneration: storageGeneration, snapshotIdentity: snapshotIdentity,
            packHash: content.digest, revision: revision)
        // Verify again after inspection/replay before returning pack-bound state.
        guard try verifiedObject(content) == object else { throw VaultError.integrity }
        return installation
    }
    public func verifiedObject(_ id: ContentID) throws -> StoredObject {
        let url = objects.appendingPathComponent(String(id.hex.prefix(2)), isDirectory: true).appendingPathComponent(id.hex)
        let (actual, length) = try digest(url)
        guard actual == id else { throw VaultError.integrity }
        return StoredObject(id: id, length: length, url: url)
    }
    private func digest(_ url: URL) throws -> (ContentID, UInt64) {
        guard try FileManager.default.attributesOfItem(atPath: url.path)[.type] as? FileAttributeType == .typeRegular else {
            throw VaultError.invalidSource
        }
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        var hasher = SHA256()
        var length: UInt64 = 0
        while let bytes = try file.read(upToCount: 64 * 1024), !bytes.isEmpty {
            try Task.checkCancellation()
            guard UInt64(bytes.count) <= UInt64(Int64.max) - length else { throw StoreError.invalidValue }
            hasher.update(data: bytes)
            length += UInt64(bytes.count)
        }
        return (try ContentID(hasher.finalize().map { String(format: "%02x", $0) }.joined()), length)
    }
}

private func syncDirectory(_ url: URL) throws {
    let descriptor = url.withUnsafeFileSystemRepresentation { path in
        guard let path else { return Int32(-1) }
        return open(path, O_RDONLY)
    }
    guard descriptor >= 0 else { throw VaultError.io(errno) }
    defer { _ = close(descriptor) }
    guard fsync(descriptor) == 0 else { throw VaultError.io(errno) }
}

// All writers share this lock; recovery requires exclusive ownership.
final class VaultLock {
    private var descriptor: Int32 = -1
    init(root: URL, exclusive: Bool, nonblocking: Bool) throws {
        let url = root.appendingPathComponent(".vault-lock")
        descriptor = url.withUnsafeFileSystemRepresentation { path in
            guard let path else { return Int32(-1) }
            return open(path, O_RDWR | O_CREAT, mode_t(0o600))
        }
        guard descriptor >= 0 else { throw VaultError.io(errno) }
        let flags = (exclusive ? LOCK_EX : LOCK_SH) | (nonblocking ? LOCK_NB : 0)
        if flock(descriptor, flags) != 0 {
            let error = errno
            release()
            throw VaultError.io(error)
        }
    }
    func release() {
        if descriptor >= 0 { _ = close(descriptor); descriptor = -1 }
    }
    deinit { release() }
}

private func reclaimStages(root: URL, staging: URL) throws -> Int {
    let lock: VaultLock
    do { lock = try VaultLock(root: root, exclusive: true, nonblocking: true) }
    catch VaultError.io(let code) where code == EWOULDBLOCK || code == EAGAIN { return 0 }
    defer { lock.release() }
    let candidates = try FileManager.default.contentsOfDirectory(at: staging, includingPropertiesForKeys: nil)
    var removed = 0
    for candidate in candidates {
        guard candidate.pathExtension == "partial",
              UUID(uuidString: candidate.deletingPathExtension().lastPathComponent) != nil,
              try FileManager.default.attributesOfItem(atPath: candidate.path)[.type] as? FileAttributeType == .typeRegular else { continue }
        try FileManager.default.removeItem(at: candidate)
        removed += 1
    }
    if removed > 0 { try syncDirectory(staging) }
    return removed
}

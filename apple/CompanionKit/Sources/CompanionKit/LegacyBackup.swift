import Foundation

public enum LegacyBackupRole: String, Codable, CaseIterable, Sendable {
    case reviews, items, profile, lessons, readings, starred, usage, days, session
}
public struct VerifiedLegacyBackup: Equatable, Identifiable, Sendable {
    public let id: ContentID
    public let manifest: LegacyBackupManifest
}
public struct LegacyBackupFile: Codable, Equatable, Sendable {
    public let role: LegacyBackupRole
    public let id: ContentID
    public let length: UInt64
    public init(role: LegacyBackupRole, id: ContentID, length: UInt64) {
        self.role = role; self.id = id; self.length = length
    }
}
public struct LegacyBackupManifest: Codable, Equatable, Sendable {
    public let version: Int
    public let reader: Data
    public let generation: Data
    public let course: Data
    public let files: [LegacyBackupFile]
    public init(reader: Data, generation: Data, course: Data, files: [LegacyBackupFile]) throws {
        version = 1; self.reader = reader; self.generation = generation; self.course = course
        self.files = files.sorted { $0.role.rawValue < $1.role.rawValue }
        try validate()
    }
    func validate() throws {
        guard version == 1, [reader, generation, course].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }),
              files.count <= LegacyBackupRole.allCases.count,
              Set(files.map(\.role)).count == files.count,
              Set(files.map(\.role)).isSuperset(of: [.reviews, .items, .profile]) else { throw ProtocolError.value }
    }
}

public extension ContentVault {
    // Expected hashes must come from an immutable reader export captured in Connect & Sync mode.
    // Persist the returned manifest ID before importing any migration events.
    func preserveLegacyBackup(_ manifest: LegacyBackupManifest, sources: [LegacyBackupRole: URL]) throws -> StoredObject {
        try manifest.validate()
        guard Set(sources.keys) == Set(manifest.files.map(\.role)) else { throw ProtocolError.value }
        for file in manifest.files {
            guard let source = sources[file.role] else { throw ProtocolError.value }
            let stored = try importFile(source)
            guard stored.id == file.id, stored.length == file.length else { throw VaultError.integrity }
        }
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let temporary = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: temporary) }
        try encoder.encode(manifest).write(to: temporary, options: [.withoutOverwriting])
        return try importFile(temporary)
    }
    func verifiedLegacyBackup(_ id: ContentID) throws -> LegacyBackupManifest {
        let object = try verifiedObject(id)
        guard object.length <= 4096 else { throw ProtocolError.value }
        let manifest = try JSONDecoder().decode(LegacyBackupManifest.self, from: Data(contentsOf: object.url))
        try manifest.validate()
        for file in manifest.files {
            guard try verifiedObject(file.id).length == file.length else { throw VaultError.integrity }
        }
        return manifest
    }
}

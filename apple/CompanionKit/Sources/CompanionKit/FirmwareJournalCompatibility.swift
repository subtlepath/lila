import Foundation

/// Journal-format compatibility only; callers must also check board, image and state schema.
public struct FirmwareJournalCompatibility: Codable, Equatable, Sendable {
    public let supportedJournalHeaderVersions: [UInt8]

    public init(supportedJournalHeaderVersions: [UInt8]) throws {
        guard supportedJournalHeaderVersions.allSatisfy({ (1 ... 3).contains($0) }),
              Set(supportedJournalHeaderVersions).count == supportedJournalHeaderVersions.count else {
            throw ProtocolError.value
        }
        self.supportedJournalHeaderVersions = supportedJournalHeaderVersions.sorted()
    }

    private enum CodingKeys: String, CodingKey { case supportedJournalHeaderVersions }

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        // Older manifests make no claim of journal support.
        let versions = try values.decodeIfPresent([UInt8].self, forKey: .supportedJournalHeaderVersions) ?? []
        try self.init(supportedJournalHeaderVersions: versions)
    }

    /// Nil means reader state was not obtained. Empty means a verified absence of journals.
    /// Include every persisted format, including retained backups and migration candidates.
    public func permits(readerHeaderVersions: [UInt8]?) -> Bool {
        guard let versions = readerHeaderVersions,
              versions.allSatisfy({ (1 ... 3).contains($0) }) else { return false }
        return versions.allSatisfy { supportedJournalHeaderVersions.contains($0) }
    }
}

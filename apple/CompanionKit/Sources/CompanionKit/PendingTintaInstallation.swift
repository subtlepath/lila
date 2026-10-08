import Foundation

public enum TintaInstallationPhase: String, Sendable { case queued, staging, committing }

public struct PendingTintaInstallation: Equatable, Sendable {
    public let transaction: UUID
    public let manifest: ContentID
    public let reader: Data
    public let storageGeneration: Data
    public let owner: Data
    public let phase: TintaInstallationPhase
    init(transaction: UUID, manifest: ContentID, reader: Data, storageGeneration: Data, owner: Data,
         phase: TintaInstallationPhase = .queued) {
        self.transaction = transaction; self.manifest = manifest; self.reader = reader
        self.storageGeneration = storageGeneration; self.owner = owner; self.phase = phase
    }
}

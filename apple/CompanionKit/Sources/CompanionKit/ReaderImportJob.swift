import Foundation

public enum ReaderImportJobPhase: String, Equatable, Sendable {
    case queued, downloading, paused, verifying, completed, aborted
}

public struct ReaderImportJob: Equatable, Sendable {
    public let id: UUID
    public let reader: Data
    public let generation: Data
    public let installation: Data
    public let manifest: ContentManifest
    public let acknowledgedOffset: UInt64
    public let phase: ReaderImportJobPhase
}

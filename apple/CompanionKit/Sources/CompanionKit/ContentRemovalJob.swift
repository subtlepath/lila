import Foundation

public enum ContentRemovalJobPhase: String, Equatable, Sendable { case queued, removing, paused, completed }

public struct ContentRemovalJob: Equatable, Sendable {
    public let id: UUID
    public let reader: Data
    public let request: ContentRemovalRequest
    public let phase: ContentRemovalJobPhase
}

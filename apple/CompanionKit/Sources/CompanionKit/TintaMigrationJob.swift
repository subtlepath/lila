import Foundation

public enum TintaMigrationJobPhase: String, Equatable, Sendable {
    case queued, admitting, transferring, committing, completed
}

public enum TintaMigrationAbortState: Int64, Equatable, Sendable { case none = 0, requested, completed }

public struct TintaMigrationJob: Equatable, Sendable {
    public var id: Data { admission.merge.transaction }
    public let backup: ContentID
    public let admission: TintaMigrationAdmission
    public let phase: TintaMigrationJobPhase
    public let acknowledgedCount: UInt32
    public let paused: Bool
    public let abortState: TintaMigrationAbortState
    init(backup: ContentID, admission: TintaMigrationAdmission, phase: TintaMigrationJobPhase,
         acknowledgedCount: UInt32, paused: Bool, abortState: TintaMigrationAbortState = .none) {
        self.backup = backup; self.admission = admission; self.phase = phase
        self.acknowledgedCount = acknowledgedCount; self.paused = paused; self.abortState = abortState
    }
}

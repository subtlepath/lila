import Foundation

public enum JournalMergePlanningError: Error, Equatable, Sendable {
    case unavailableCourse(Data), missingContent(Data)
}

public enum JournalMergeJobPhase: String, Equatable, Sendable {
    case queued, transferring, committing, completed
}

public enum JournalMergeAbortState: Int64, Equatable, Sendable { case none = 0, requested, completed }

public struct JournalMergeJob: Equatable, Sendable {
    public var id: Data { declaration.transaction }
    public let reader: Data
    public let declaration: JournalMergeDeclaration
    public let phase: JournalMergeJobPhase
    public let acknowledgedCount: UInt32
    public let paused: Bool
    public let abortState: JournalMergeAbortState
    init(reader: Data, declaration: JournalMergeDeclaration, phase: JournalMergeJobPhase,
         acknowledgedCount: UInt32, paused: Bool, abortState: JournalMergeAbortState = .none) {
        self.reader = reader; self.declaration = declaration; self.phase = phase
        self.acknowledgedCount = acknowledgedCount; self.paused = paused; self.abortState = abortState
    }
}

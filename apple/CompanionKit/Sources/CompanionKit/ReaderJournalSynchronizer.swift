import Foundation

public struct VerifiedReaderJournalCheckpoint: Equatable, Sendable {
    public let reader: Data
    public let generation: Data
    public let snapshot: JournalMergeSnapshot
    public let mutations: [JournalMutation]
}

public enum ReaderJournalSyncPreparation: Equatable, Sendable {
    case cancellationPending(JournalMergeJob), upload(JournalMergeJob), upToDate(VerifiedReaderJournalCheckpoint), blocked(JournalMergeReadiness), installationPending, upgradeRequired
}
public enum ReaderJournalSynchronizerError: Error, Equatable, Sendable {
    case busy, invalidSnapshot, requestIDsExhausted
}

public actor ReaderJournalSynchronizer {
    private let exports = JournalExportCollector()
    private var busy = false
    private var requestID: UInt32 = 0
    public init() {}

    public func prepare(session: AuthenticatedReaderSession, inventory: ReaderInventory,
                        library: LibraryStore, maximumEvents: Int = 100_000) async throws -> ReaderJournalSyncPreparation {
        try await prepare(reader: session.device.identity, generation: session.device.storageGeneration,
            owner: session.installation, capabilities: session.device.capabilities, transport: session,
            inventory: inventory, library: library, maximumEvents: maximumEvents)
    }
    func prepare(reader: Data, generation: Data, owner: Data, capabilities: UInt32,
                 transport: any CompanionTransport, inventory: ReaderInventory,
                 library: LibraryStore, maximumEvents: Int = 100_000) async throws -> ReaderJournalSyncPreparation {
        guard !busy else { throw ReaderJournalSynchronizerError.busy }
        guard capabilities & JournalExportPage.capability != 0 else { throw ReaderSessionError.unsupportedProtocol }
        guard inventory.complete, inventory.reader == reader, inventory.generation == generation,
              owner.count == 16, owner.contains(where: { $0 != 0 }) else { throw ReaderSessionError.wrongReader }
        guard maximumEvents >= 0, maximumEvents <= Int(UInt32.max) else { throw JournalExportCollectorError.invalidLimit }
        busy = true
        defer { busy = false }
        try Task.checkCancellation()
        // An open candidate excludes export; recover its exact saved transaction first.
        if let pending = try await library.pendingJournalMerge(reader: reader, generation: generation) {
            guard pending.declaration.owner == owner else { throw ReaderSessionError.wrongReader }
            guard capabilities & JournalMergeReadiness.capability != 0 else { return .upgradeRequired }
            _ = try await library.journalMergeMutations(pending)
            if pending.abortState == .requested { return .cancellationPending(pending) }
            return .upload(pending)
        }
        if try await library.pendingTintaMigration(reader: reader, generation: generation) != nil {
            return .installationPending
        }
        if try await library.pendingTintaInstallation(reader: reader, generation: generation) != nil {
            return .installationPending
        }
        let exported = try await exports.collect(reader: reader, generation: generation, capabilities: capabilities,
            transport: transport, library: library, maximumEvents: maximumEvents)
        try Task.checkCancellation()
        guard capabilities & JournalMergeReadiness.capability != 0 else { return .upgradeRequired }
        let stateRequest = try JournalState.request(generation: generation, requestID: nextRequestID())
        let snapshot = try JournalState.decode(await transport.exchange(stateRequest), request: stateRequest)
        guard snapshot.count == exported.count, snapshot.frontier == exported.frontier else {
            throw ReaderJournalSynchronizerError.invalidSnapshot
        }
        try Task.checkCancellation()
        let readinessRequest = try JournalMergeReadiness.request(generation: generation, snapshot: snapshot, requestID: nextRequestID())
        let readiness = try JournalMergeReadiness.decode(await transport.exchange(readinessRequest), request: readinessRequest)
        guard readiness == .ready || readiness == .noCourse else { return .blocked(readiness) }
        try Task.checkCancellation()
        if let job = try await library.prepareReaderJournalMerge(owner: owner, previous: exported.mutations,
            snapshot: snapshot, inventory: inventory) { return .upload(job) }
        return .upToDate(VerifiedReaderJournalCheckpoint(reader: reader, generation: generation,
            snapshot: snapshot, mutations: exported.mutations))
    }
    private func nextRequestID() throws -> UInt32 {
        guard requestID < UInt32.max else { throw ReaderJournalSynchronizerError.requestIDsExhausted }
        requestID += 1
        return requestID
    }
}

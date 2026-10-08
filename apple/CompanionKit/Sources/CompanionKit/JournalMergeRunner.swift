import Foundation

public enum JournalMergeRunnerError: Error, Equatable, Sendable {
    case busy, abortRequested, rejected(JournalMergeResult), invalidCount, requestIDsExhausted
}

public actor JournalMergeRunner {
    private var busy = false
    private var requestID: UInt32 = 0
    public init() {}

    public func run(transaction: Data, session: AuthenticatedReaderSession, library: LibraryStore) async throws -> JournalMergeJob {
        guard session.device.capabilities & (JournalExportPage.capability | JournalMergeReadiness.capability) ==
            (JournalExportPage.capability | JournalMergeReadiness.capability) else { throw ReaderSessionError.unsupportedProtocol }
        return try await run(transaction: transaction, reader: session.device.identity,
            generation: session.device.storageGeneration, owner: session.installation, transport: session, library: library)
    }
    public func run(transaction: Data, wifi: WifiHandoffTransport, library: LibraryStore) async throws -> JournalMergeJob {
        let binding = await wifi.journalMergeBinding()
        guard transaction == binding.3 else { throw WifiHandoffTransportError.binding }
        return try await run(transaction: transaction, reader: binding.0, generation: binding.1,
            owner: binding.2, transport: WifiJournalMergeTransport(handoff: wifi), library: library)
    }
    func run(transaction: Data, reader: Data, generation: Data, owner: Data,
             transport: any CompanionTransport, library: LibraryStore) async throws -> JournalMergeJob {
        guard !busy else { throw JournalMergeRunnerError.busy }
        busy = true
        defer { busy = false }
        guard var job = try await library.journalMergeJob(transaction) else { throw StoreError.missingJob }
        guard job.reader == reader, job.declaration.generation == generation,
              job.declaration.owner == owner else { throw ReaderSessionError.wrongReader }
        guard job.abortState == .none else { throw JournalMergeRunnerError.abortRequested }
        if job.phase == .completed { return job }
        let mutations = try await library.journalMergeMutations(job)
        do {
            try Task.checkCancellation()
            job = try await prepare(job, transport: transport, library: library)
            let declaration = job.declaration
            if job.phase == .transferring {
                let start = Int(job.acknowledgedCount - declaration.previous.count)
                for index in start..<mutations.count {
                    try Task.checkCancellation()
                    let reply = try await exchange(.append(transaction: transaction, mutation: mutations[index]), transport: transport)
                    try accepted(reply)
                    let expected = declaration.previous.count + UInt32(index) + 1
                    guard reply.count == expected else { throw JournalMergeRunnerError.invalidCount }
                    job = try await library.updateJournalMerge(job, phase: .transferring, acknowledgedCount: reply.count)
                }
                job = try await library.updateJournalMerge(job, phase: .committing, acknowledgedCount: job.acknowledgedCount)
            }
            try Task.checkCancellation()
            let committed = try await exchange(.commit(declaration), transport: transport)
            try accepted(committed)
            guard committed.count == declaration.merged.count else { throw JournalMergeRunnerError.invalidCount }
            return try await library.updateJournalMerge(job, phase: .completed, acknowledgedCount: committed.count)
        } catch {
            if let current = try? await library.journalMergeJob(transaction) {
                _ = try? await library.updateJournalMerge(current, phase: current.phase,
                    acknowledgedCount: current.acknowledgedCount, paused: true)
            }
            throw error
        }
    }
    public func abort(transaction: Data, session: AuthenticatedReaderSession,
                      library: LibraryStore) async throws -> JournalMergeJob {
        guard session.device.capabilities & (JournalExportPage.capability | JournalMergeReadiness.capability) ==
            (JournalExportPage.capability | JournalMergeReadiness.capability) else { throw ReaderSessionError.unsupportedProtocol }
        return try await abort(transaction: transaction, reader: session.device.identity,
            generation: session.device.storageGeneration, owner: session.installation, transport: session, library: library)
    }
    func abort(transaction: Data, reader: Data, generation: Data, owner: Data,
               transport: any CompanionTransport, library: LibraryStore) async throws -> JournalMergeJob {
        guard !busy else { throw JournalMergeRunnerError.busy }
        busy = true
        defer { busy = false }
        try Task.checkCancellation()
        guard let job = try await library.journalMergeJob(transaction) else { throw StoreError.missingJob }
        guard job.reader == reader, job.declaration.generation == generation,
              job.declaration.owner == owner else { throw ReaderSessionError.wrongReader }
        if job.abortState == .completed || job.phase == .completed { return job }
        let requested = try await library.requestJournalMergeAbort(job)
        try Task.checkCancellation()
        if requested.phase == .committing {
            let begun = try await exchange(.begin(job.declaration), transport: transport)
            try accepted(begun)
            guard begun.count == requested.declaration.merged.count else { throw JournalMergeRunnerError.invalidCount }
            if begun.result == .duplicate {
                let receipt = try await exchange(.commit(job.declaration), transport: transport)
                try accepted(receipt)
                guard receipt.count == requested.declaration.merged.count else { throw JournalMergeRunnerError.invalidCount }
                return try await library.completeCommittedJournalMerge(requested)
            }
        }
        try Task.checkCancellation()
        let reply = try await exchange(.abort(job.declaration), transport: transport)
        guard reply.result == .ok else { throw JournalMergeRunnerError.rejected(reply.result) }
        return try await library.completeJournalMergeAbort(requested)
    }
    public func prepareForHandoff(transaction: Data, session: AuthenticatedReaderSession,
                                  library: LibraryStore) async throws -> JournalMergeJob {
        guard session.device.capabilities & (JournalExportPage.capability | JournalMergeReadiness.capability) ==
            (JournalExportPage.capability | JournalMergeReadiness.capability) else { throw ReaderSessionError.unsupportedProtocol }
        return try await prepareForHandoff(transaction: transaction, reader: session.device.identity,
            generation: session.device.storageGeneration, owner: session.installation, transport: session, library: library)
    }
    func prepareForHandoff(transaction: Data, reader: Data, generation: Data, owner: Data,
                           transport: any CompanionTransport, library: LibraryStore) async throws -> JournalMergeJob {
        guard !busy else { throw JournalMergeRunnerError.busy }
        busy = true
        defer { busy = false }
        guard let job = try await library.journalMergeJob(transaction) else { throw StoreError.missingJob }
        guard job.reader == reader, job.declaration.generation == generation,
              job.declaration.owner == owner else { throw ReaderSessionError.wrongReader }
        guard job.abortState == .none else { throw JournalMergeRunnerError.abortRequested }
        if job.phase == .completed { return job }
        _ = try await library.journalMergeMutations(job)
        do {
            try Task.checkCancellation()
            return try await prepare(job, transport: transport, library: library)
        } catch {
            if let current = try? await library.journalMergeJob(transaction) {
                _ = try? await library.updateJournalMerge(current, phase: current.phase,
                    acknowledgedCount: current.acknowledgedCount, paused: true)
            }
            throw error
        }
    }
    private func prepare(_ saved: JournalMergeJob, transport: any CompanionTransport,
                         library: LibraryStore) async throws -> JournalMergeJob {
        var job = saved
        if job.paused { job = try await library.updateJournalMerge(job, phase: job.phase, acknowledgedCount: job.acknowledgedCount) }
        if job.phase == .queued {
            job = try await library.updateJournalMerge(job, phase: .transferring, acknowledgedCount: job.acknowledgedCount)
        }
        try Task.checkCancellation()
        let declaration = job.declaration
        let begun = try await exchange(.begin(declaration), transport: transport)
        try accepted(begun)
        guard begun.count >= job.acknowledgedCount, begun.count <= declaration.merged.count,
              begun.result != .duplicate || begun.count == declaration.merged.count else {
            throw JournalMergeRunnerError.invalidCount
        }
        job = try await library.updateJournalMerge(job, phase: job.phase, acknowledgedCount: begun.count)
        return job
    }
    private func nextRequestID() throws -> UInt32 {
        guard requestID < UInt32.max else { throw JournalMergeRunnerError.requestIDsExhausted }
        requestID += 1
        return requestID
    }
    private func exchange(_ operation: JournalMergeRequest, transport: any CompanionTransport) async throws -> JournalMergeReply {
        let request = try operation.frame(requestID: nextRequestID())
        return try JournalMergeReply.decode(await transport.exchange(request), request: request)
    }
    private func accepted(_ reply: JournalMergeReply) throws {
        guard reply.result == .ok || reply.result == .duplicate else { throw JournalMergeRunnerError.rejected(reply.result) }
    }
}

private struct WifiJournalMergeTransport: CompanionTransport {
    let handoff: WifiHandoffTransport
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        try await handoff.exchangeJournalMergeControl(request)
    }
}

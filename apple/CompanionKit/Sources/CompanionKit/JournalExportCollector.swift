import Foundation

public enum JournalExportCollectorError: Error, Equatable, Sendable {
    case busy, invalidLimit, limitExceeded, duplicateIdentity, requestIDsExhausted
}

public struct JournalExportReceipt: Equatable, Sendable {
    public let frontier: Data
    public let count: UInt32
    public let inserted: Int
    public let mutations: [JournalMutation]
}

public actor JournalExportCollector {
    private var busy = false
    private var requestID: UInt32 = 0
    public init() {}

    public func collect(wifi: WifiHandoffTransport, library: LibraryStore, maximumEvents: Int) async throws -> JournalExportReceipt {
        let binding = await wifi.journalReaderBinding()
        return try await collect(reader: binding.0, generation: binding.1, capabilities: JournalExportPage.capability, transport: WifiJournalExportTransport(handoff: wifi),
                          library: library, maximumEvents: maximumEvents)
    }

    public func collect(session: AuthenticatedReaderSession, library: LibraryStore, maximumEvents: Int) async throws -> JournalExportReceipt {
        try await collect(reader: session.device.identity, generation: session.device.storageGeneration,
                          capabilities: session.device.capabilities, transport: session, library: library, maximumEvents: maximumEvents)
    }

    func collect(reader: Data, generation: Data, capabilities: UInt32, transport: any CompanionTransport, library: LibraryStore, maximumEvents: Int) async throws -> JournalExportReceipt {
        guard !busy else { throw JournalExportCollectorError.busy }
        guard reader.count == 16, reader.contains(where: { $0 != 0 }), generation.count == 16,
              generation.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
        guard maximumEvents >= 0, maximumEvents <= Int(UInt32.max) else { throw JournalExportCollectorError.invalidLimit }
        guard capabilities & JournalExportPage.capability != 0 else { throw ReaderSessionError.unsupportedProtocol }
        busy = true
        defer { busy = false }
        var cursor = JournalExportCursor.start
        var mutations: [JournalMutation] = []
        var identities = Set<EventIdentity>()
        var initialized = false
        while true {
            try Task.checkCancellation()
            guard requestID < UInt32.max else { throw JournalExportCollectorError.requestIDsExhausted }
            requestID += 1
            let request = try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: cursor.payload)
            let reply = try await transport.exchange(request)
            let page = try JournalExportPage.decode(reply, requestID: requestID, requested: cursor)
            guard Int(page.cursor.count) <= maximumEvents else { throw JournalExportCollectorError.limitExceeded }
            if !initialized {
                mutations.reserveCapacity(Int(page.cursor.count))
                identities.reserveCapacity(Int(page.cursor.count))
                initialized = true
            }
            cursor = page.cursor
            if let mutation = page.mutation {
                guard identities.insert(mutation.event.identity).inserted else { throw JournalExportCollectorError.duplicateIdentity }
                mutations.append(mutation)
            } else { break }
        }
        guard mutations.count == Int(cursor.count), try TintaJournalFrontier.digest(mutations) == cursor.frontier else {
            throw HistoryError.staleFrontier
        }
        try Task.checkCancellation()
        // No await occurs inside the store's transaction; failures roll back every event.
        let inserted = try await library.importReaderJournal(mutations, reader: reader, generation: generation,
                                                            frontier: cursor.frontier, count: cursor.count)
        return JournalExportReceipt(frontier: cursor.frontier, count: cursor.count, inserted: inserted, mutations: mutations)
    }
}

private struct WifiJournalExportTransport: CompanionTransport {
    let handoff: WifiHandoffTransport
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        try await handoff.exchangeJournalExport(request)
    }
}

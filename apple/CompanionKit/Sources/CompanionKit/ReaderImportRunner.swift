import Foundation

public enum ReaderImportRunnerError: Error, Equatable, Sendable {
    case busy, wrongReader, wrongStorage, wrongInstallation, incompleteInventory, unsupportedProtocol, unsupportedContent, aborted, requestIDsExhausted, wrongHandoff
    case rejected(ReaderContentReadResult)
}

public struct ReaderImportCleanupReport: Equatable, Sendable {
    public let checkedJobs: Int
    public let failedJobs: [UUID]
    public let nextCursor: UUID?
}

public actor ReaderImportRunner {
    private let library: LibraryStore
    private let storage: ReaderImportStorage
    private let importer: ContentImporter
    private var running = false
    private var requestID: UInt32 = 0
    public init(library: LibraryStore, storage: ReaderImportStorage, importer: ContentImporter) {
        self.library = library; self.storage = storage; self.importer = importer
    }
    public func cleanupFinishedImports(after: UUID? = nil, limit: Int = 32) async throws -> ReaderImportCleanupReport {
        guard !running else { throw ReaderImportRunnerError.busy }
        running = true
        defer { running = false }
        let jobs = try await library.finishedReaderImports(after: after, limit: limit)
        var failed: [UUID] = []; failed.reserveCapacity(jobs.count)
        for job in jobs {
            try Task.checkCancellation()
            do { try await storage.discard(job) }
            catch { failed.append(job.id) }
        }
        return ReaderImportCleanupReport(checkedJobs: jobs.count, failedJobs: failed,
                                         nextCursor: jobs.count == limit ? jobs.last?.id : nil)
    }
    public func cancel(_ id: UUID) async throws -> ReaderImportJob {
        guard !running else { throw ReaderImportRunnerError.busy }
        running = true
        defer { running = false }
        try await library.abortReaderImport(id)
        guard let job = try await library.readerImportJob(id), job.phase == .aborted else { throw StoreError.invalidTransition }
        try await storage.discard(job)
        return job
    }
    public func prepareImport(manifest: ContentManifest, session: AuthenticatedReaderSession,
                              inventory: ReaderInventory, resuming id: UUID? = nil) async throws -> ReaderImportJob {
        guard !running else { throw ReaderImportRunnerError.busy }
        running = true
        defer { running = false }
        guard session.device.readerCapabilities.supportsContentMetadata else { throw ReaderImportRunnerError.unsupportedContent }
        guard inventory.reader == session.device.identity else { throw ReaderImportRunnerError.wrongReader }
        guard inventory.generation == session.device.storageGeneration else { throw ReaderImportRunnerError.wrongStorage }
        guard inventory.complete, inventory.contents.contains(manifest) else { throw ReaderImportRunnerError.incompleteInventory }
        if let id { _ = try await matchingImport(id, manifest: manifest, inventory: inventory, installation: session.installation) }
        let request = try ReaderContentMetadataRequest(generation: inventory.generation, manifest: manifest)
        guard requestID < UInt32.max else { throw ReaderImportRunnerError.requestIDsExhausted }
        requestID += 1
        let frame = try ControlFrame(command: .contentMetadata, requestID: requestID, payload: request.encoded)
        try Task.checkCancellation()
        let response = try await session.exchange(frame)
        guard response.response, response.command == frame.command, response.requestID == frame.requestID else { throw ProtocolError.value }
        let reply = try ReaderContentMetadataReply(decoding: response.payload, request: request)
        guard reply.result == .ok else { throw ReaderImportRunnerError.rejected(reply.result) }
        try Task.checkCancellation()
        let job: ReaderImportJob
        if let id {
            job = try await matchingImport(id, manifest: manifest, inventory: inventory, installation: session.installation)
        } else {
            job = try await library.enqueueReaderImport(manifest: manifest, inventory: inventory, installation: session.installation)
        }
        _ = try await library.bindReaderImportFilename(job.id, request: request, reply: reply)
        return job
    }
    private func matchingImport(_ id: UUID, manifest: ContentManifest, inventory: ReaderInventory,
                                installation: Data) async throws -> ReaderImportJob {
        guard let job = try await library.readerImportJob(id), job.reader == inventory.reader,
              job.generation == inventory.generation, job.installation == installation,
              job.manifest == manifest, job.phase != .completed, job.phase != .aborted else {
            throw StoreError.invalidTransition
        }
        return job
    }
    public func prepareForHandoff(_ id: UUID, session: AuthenticatedReaderSession,
                                  inventory: ReaderInventory) async throws -> ReaderImportJob {
        guard !running else { throw ReaderImportRunnerError.busy }
        running = true
        defer { running = false }
        guard session.device.readerCapabilities.supportsWifiContentRead else { throw ReaderImportRunnerError.unsupportedContent }
        guard let job = try await library.readerImportJob(id) else { throw StoreError.missingJob }
        guard job.reader == session.device.identity, inventory.reader == job.reader else { throw ReaderImportRunnerError.wrongReader }
        guard job.generation == session.device.storageGeneration, inventory.generation == job.generation else {
            throw ReaderImportRunnerError.wrongStorage
        }
        guard job.installation == session.installation else { throw ReaderImportRunnerError.wrongInstallation }
        guard inventory.complete, inventory.contents.contains(job.manifest) else { throw ReaderImportRunnerError.incompleteInventory }
        guard job.phase == .queued || job.phase == .paused || job.phase == .downloading,
              try await library.readerImportFilename(id) != nil else { throw StoreError.invalidTransition }
        _ = try await storage.prepare(job)
        let request = try ReaderContentHandoffRequest(transaction: withUnsafeBytes(of: id.uuid) { Data($0) },
            generation: job.generation, manifest: job.manifest, offset: job.acknowledgedOffset)
        guard requestID < UInt32.max else { throw ReaderImportRunnerError.requestIDsExhausted }
        requestID += 1
        let frame = try ControlFrame(command: .prepareContentHandoff, requestID: requestID, payload: request.encoded)
        let response = try await session.exchange(frame)
        guard response.response, response.command == frame.command, response.requestID == frame.requestID else { throw ProtocolError.value }
        let reply = try ReaderContentHandoffReply(decoding: response.payload, request: request)
        guard reply.result == .ok else { throw ReaderImportRunnerError.rejected(reply.result) }
        try Task.checkCancellation()
        guard try await library.readerImportJob(id) == job else { throw StoreError.invalidTransition }
        return job
    }
    public func download(_ id: UUID, session: AuthenticatedReaderSession,
                         inventory: ReaderInventory) async throws -> LibraryContent {
        guard let filename = try await library.readerImportFilename(id) else { throw StoreError.invalidTransition }
        return try await download(id, session: session, inventory: inventory, originalFilename: filename)
    }
    public func download(_ id: UUID, session: AuthenticatedReaderSession, inventory: ReaderInventory,
                         originalFilename: String) async throws -> LibraryContent {
        guard session.device.readerCapabilities.supportsContentRead else { throw ReaderImportRunnerError.unsupportedContent }
        return try await download(id, device: session.device, inventory: inventory, installation: session.installation,
            originalFilename: originalFilename) { request in try await self.read(request, session: session) }
    }
    public func download(_ id: UUID, session: AuthenticatedReaderSession, inventory: ReaderInventory,
                         handoff: WifiHandoffTransport) async throws -> LibraryContent {
        do {
            guard session.device.readerCapabilities.supportsWifiContentRead else { throw ReaderImportRunnerError.unsupportedContent }
            guard let job = try await library.readerImportJob(id) else { throw StoreError.missingJob }
            guard job.reader == session.device.identity else { throw ReaderImportRunnerError.wrongReader }
            guard job.generation == session.device.storageGeneration else { throw ReaderImportRunnerError.wrongStorage }
            guard job.installation == session.installation else { throw ReaderImportRunnerError.wrongInstallation }
            let transaction = withUnsafeBytes(of: id.uuid) { Data($0) }
            guard await handoff.matches(reader: job.reader, storageGeneration: job.generation,
                installation: job.installation, transaction: transaction) else { throw ReaderImportRunnerError.wrongHandoff }
            guard let filename = try await library.readerImportFilename(id) else { throw StoreError.invalidTransition }
            return try await download(id, device: session.device, inventory: inventory, installation: session.installation,
                originalFilename: filename) { request in
                    try await self.read(request, transaction: transaction, handoff: handoff)
                }
        } catch {
            await handoff.close()
            throw error
        }
    }
    private func read(_ request: ReaderContentReadRequest, transaction: Data,
                      handoff: WifiHandoffTransport) async throws -> Data {
        guard requestID < UInt32.max else { throw ReaderImportRunnerError.requestIDsExhausted }
        requestID += 1
        let frame = try ControlFrame(command: .readContent, requestID: requestID, payload: transaction + request.encoded)
        let reply = try await handoff.exchange(frame)
        guard reply.command == .readContent else { throw WifiHandoffTransportError.invalidResponse }
        return reply.payload
    }
    private func read(_ request: ReaderContentReadRequest, session: AuthenticatedReaderSession) async throws -> Data {
        guard requestID < UInt32.max else { throw ReaderImportRunnerError.requestIDsExhausted }
        requestID += 1
        let frame = try ControlFrame(command: .readContent, requestID: requestID, payload: request.encoded)
        let reply = try await session.exchange(frame)
        guard reply.response, reply.command == frame.command, reply.requestID == frame.requestID else { throw ProtocolError.value }
        return reply.payload
    }
    // Internal transport entry point for protocol and persistence tests.
    func download(_ id: UUID, device: DeviceDescriptor, inventory: ReaderInventory, installation: Data,
                  originalFilename: String,
                  read: @Sendable (ReaderContentReadRequest) async throws -> Data) async throws -> LibraryContent {
        guard !running else { throw ReaderImportRunnerError.busy }
        running = true
        defer { running = false }
        guard var job = try await library.readerImportJob(id) else { throw StoreError.missingJob }
        if let bound = try await library.readerImportFilename(id), bound != originalFilename {
            throw StoreError.conflictingJob
        }
        guard job.reader == device.identity, inventory.reader == device.identity else { throw ReaderImportRunnerError.wrongReader }
        guard job.generation == device.storageGeneration, inventory.generation == device.storageGeneration else {
            throw ReaderImportRunnerError.wrongStorage
        }
        guard job.installation == installation else { throw ReaderImportRunnerError.wrongInstallation }
        guard device.minimumProtocol <= 1, device.maximumProtocol >= 1 else { throw ReaderImportRunnerError.unsupportedProtocol }
        if job.phase == .completed {
            return try await importer.finishReaderImport(id, storage: storage, originalFilename: originalFilename)
        }
        guard job.phase != .aborted else { throw ReaderImportRunnerError.aborted }
        guard inventory.complete, inventory.contents.contains(job.manifest) else { throw ReaderImportRunnerError.incompleteInventory }
        do {
            try Task.checkCancellation()
            _ = try await storage.prepare(job)
            if job.phase == .queued || job.phase == .paused {
                try await library.checkpointReaderImport(id, offset: job.acknowledgedOffset, phase: .downloading)
            }
            while true {
                try Task.checkCancellation()
                guard let saved = try await library.readerImportJob(id) else { throw StoreError.missingJob }
                guard saved.phase != .aborted else { throw ReaderImportRunnerError.aborted }
                guard saved.phase == .downloading || saved.phase == .verifying else { throw StoreError.invalidTransition }
                job = saved
                if job.acknowledgedOffset == job.manifest.length { break }
                guard job.phase == .downloading else { throw StoreError.invalidTransition }
                let request = try ReaderContentReadRequest(generation: job.generation, manifest: job.manifest,
                    offset: job.acknowledgedOffset, maximumBytes: UInt16(ReaderContentReadRequest.maximumChunkBytes))
                let reply = try ReaderContentReadReply(decoding: await read(request), request: request)
                guard reply.result == .ok else { throw ReaderImportRunnerError.rejected(reply.result) }
                try Task.checkCancellation()
                guard let fresh = try await library.readerImportJob(id) else { throw StoreError.missingJob }
                guard fresh.phase != .aborted else { throw ReaderImportRunnerError.aborted }
                guard fresh == job else { throw StoreError.invalidTransition }
                let offset = try await storage.append(reply.bytes, to: job)
                try await library.checkpointReaderImport(id, offset: offset, phase: .downloading)
            }
            try Task.checkCancellation()
            if job.phase != .verifying {
                try await library.checkpointReaderImport(id, offset: job.acknowledgedOffset, phase: .verifying)
            }
            return try await importer.finishReaderImport(id, storage: storage, originalFilename: originalFilename)
        } catch {
            if let current = try? await library.readerImportJob(id), current.phase == .downloading || current.phase == .verifying {
                try? await library.checkpointReaderImport(id, offset: current.acknowledgedOffset, phase: .paused)
            }
            throw error
        }
    }
}

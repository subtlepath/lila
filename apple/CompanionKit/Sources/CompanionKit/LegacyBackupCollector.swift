import Foundation

public enum LegacyBackupCollectorError: Error, Equatable, Sendable {
    case busy, rejected, limit, requestIDsExhausted, conflictingStage
}

public struct LegacyBackupProgress: Equatable, Sendable {
    public let role: LegacyBackupRole
    public let completed: UInt64
    public let total: UInt64
}

public actor LegacyBackupCollector {
    private var busy = false
    private var requestID: UInt32 = 0
    public init() {}
    public func collect(wifi: WifiHandoffTransport, course: Data, transaction: Data, bound: Bool,
                        capture: Bool, staging: URL, library: LibraryStore, vault: ContentVault,
                        maximumBytes: UInt64 = 64 * 1024 * 1024,
                        progress: (@Sendable (LegacyBackupProgress) async throws -> Void)? = nil) async throws -> ContentID {
        let binding = await wifi.legacyBackupBinding()
        guard transaction == binding.2 else { throw WifiHandoffTransportError.binding }
        return try await collect(reader: binding.0, generation: binding.1,
            transport: WifiLegacyBackupTransport(handoff: wifi), course: course, transaction: transaction,
            bound: bound, capture: capture, staging: staging, library: library, vault: vault, maximumBytes: maximumBytes, progress: progress)
    }
    public func collect(session: AuthenticatedReaderSession, course: Data, transaction: Data, bound: Bool,
                        capture: Bool, staging: URL, library: LibraryStore, vault: ContentVault,
                        maximumBytes: UInt64 = 64 * 1024 * 1024,
                        progress: (@Sendable (LegacyBackupProgress) async throws -> Void)? = nil) async throws -> ContentID {
        try await collect(reader: session.device.identity, generation: session.device.storageGeneration,
            transport: session, course: course, transaction: transaction, bound: bound, capture: capture,
            staging: staging, library: library, vault: vault, maximumBytes: maximumBytes, progress: progress)
    }
    func collect(reader: Data, generation: Data, transport: any CompanionTransport, course: Data,
                 transaction: Data, bound: Bool, capture: Bool, staging: URL, library: LibraryStore,
                 vault: ContentVault, maximumBytes: UInt64,
                 progress: (@Sendable (LegacyBackupProgress) async throws -> Void)? = nil) async throws -> ContentID {
        guard !busy else { throw LegacyBackupCollectorError.busy }
        guard reader.count == 16, reader.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        busy = true
        defer { busy = false }
        func operation(_ op: LegacyBackupRequest.Operation, role: LegacyBackupRole? = nil,
                       offset: UInt32 = 0, count: UInt16 = 0) throws -> LegacyBackupRequest {
            try LegacyBackupRequest(operation: op, role: role, bound: bound, course: course,
                transaction: transaction, generation: generation, offset: offset, count: count)
        }
        if let completed = try await library.completedLegacyBackupExport(reader: reader, generation: generation,
            course: course, transaction: transaction, vault: vault) {
            var total: UInt64 = 0
            for file in completed.manifest.files {
                guard file.length <= maximumBytes - min(total, maximumBytes) else { throw LegacyBackupCollectorError.limit }
                total += file.length
            }
            try Task.checkCancellation()
            return completed.id
        }
        do {
            if capture { _ = try await send(operation(.capture), transport: transport) }
            let manifestReply = try await send(operation(.manifest), transport: transport)
            let export = try LegacyReaderBackupManifest(decoding: manifestReply.body)
            let manifest = export.manifest
            guard manifest.reader == reader else { throw ReaderSessionError.wrongReader }
            var total: UInt64 = 0
            for file in manifest.files {
                guard file.length <= maximumBytes - min(total, maximumBytes) else { throw LegacyBackupCollectorError.limit }
                total += file.length
            }
            let key = [reader, generation, course, transaction].map { $0.map { String(format: "%02x", $0) }.joined() }.joined(separator: "-")
            let directory = staging.appendingPathComponent(key, isDirectory: true)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let savedManifest = directory.appendingPathComponent("manifest")
            if !FileManager.default.fileExists(atPath: savedManifest.path) {
                try Data().write(to: savedManifest, options: .withoutOverwriting)
            }
            let manifestHandle = try FileHandle(forUpdating: savedManifest)
            do {
                let length = try manifestHandle.seekToEnd()
                guard length <= UInt64(manifestReply.body.count) else { throw LegacyBackupCollectorError.conflictingStage }
                try manifestHandle.seek(toOffset: 0)
                let prefix = try manifestHandle.read(upToCount: Int(length)) ?? Data()
                guard prefix.count == Int(length), manifestReply.body.starts(with: prefix) else {
                    throw LegacyBackupCollectorError.conflictingStage
                }
                try manifestHandle.seek(toOffset: length)
                try manifestHandle.write(contentsOf: manifestReply.body.dropFirst(Int(length)))
                try manifestHandle.synchronize()
                try manifestHandle.close()
            } catch {
                try? manifestHandle.close()
                throw error
            }
            var sources: [LegacyBackupRole: URL] = [:]
            var completedBytes: UInt64 = 0
            for file in manifest.files {
                try Task.checkCancellation()
                let path = directory.appendingPathComponent(file.role.rawValue)
                if !FileManager.default.fileExists(atPath: path.path) {
                    guard FileManager.default.createFile(atPath: path.path, contents: nil) else { throw VaultError.invalidSource }
                }
                let handle = try FileHandle(forUpdating: path)
                do {
                    var offset = try handle.seekToEnd()
                    guard offset <= file.length else { throw LegacyBackupCollectorError.conflictingStage }
                    while true {
                        let reply = try await send(operation(.file, role: file.role, offset: UInt32(offset), count: 768), transport: transport)
                        guard offset + UInt64(reply.body.count) <= file.length,
                              reply.final == (offset + UInt64(reply.body.count) == file.length) else { throw VaultError.integrity }
                        try handle.write(contentsOf: reply.body)
                        try handle.synchronize()
                        offset += UInt64(reply.body.count)
                        try await progress?(LegacyBackupProgress(role: file.role, completed: completedBytes + offset, total: total))
                        if reply.final { break }
                    }
                    try handle.close()
                } catch {
                    try? handle.close()
                    throw error
                }
                sources[file.role] = path
                completedBytes += file.length
            }
            _ = try await send(operation(.close), transport: transport)
            try Task.checkCancellation()
            return try await library.preserveLegacyBackupExport(export, sources: sources, vault: vault)
        } catch {
            if let close = try? operation(.close) { _ = try? await send(close, transport: transport, checkCancellation: false) }
            throw error
        }
    }
    private func send(_ operation: LegacyBackupRequest, transport: any CompanionTransport,
                      checkCancellation: Bool = true) async throws -> LegacyBackupReply {
        if checkCancellation { try Task.checkCancellation() }
        guard requestID < UInt32.max else { throw LegacyBackupCollectorError.requestIDsExhausted }
        requestID += 1
        let request = try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: operation.bytes)
        let reply = try LegacyBackupReply.decode(await transport.exchange(request), request: request)
        guard reply.result == .ok else { throw LegacyBackupCollectorError.rejected }
        return reply
    }
}

private struct WifiLegacyBackupTransport: CompanionTransport {
    let handoff: WifiHandoffTransport
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        try await handoff.exchangeLegacyBackup(request)
    }
}

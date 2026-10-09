import Foundation

public struct ContentManifest: Equatable, Sendable {
    public let content: ContentID
    public let kind: ContentKind
    public let length: UInt64
    public let formatVersion: UInt32
    public let logicalIdentity: Data
    public init(content: ContentID, kind: ContentKind, length: UInt64,
                formatVersion: UInt32, logicalIdentity: Data) throws {
        guard logicalIdentity.count == 16 else { throw ProtocolError.value }
        self.content = content; self.kind = kind; self.length = length
        self.formatVersion = formatVersion; self.logicalIdentity = logicalIdentity
    }
    public var encoded: Data {
        var bytes = Data([1, 2]); bytes.reserveCapacity(63)
        bytes.append(content.digest); bytes.append(UInt8(kind.rawValue))
        bytes.appendLittleEndian(length, count: 8); bytes.appendLittleEndian(UInt64(formatVersion), count: 4)
        bytes.append(logicalIdentity)
        return bytes
    }
    public init(decoding bytes: Data) throws {
        guard try RecordEnvelope(decoding: bytes).kind == .contentManifest else { throw ProtocolError.record }
        var reader = ByteReader(bytes); _ = try reader.take(2)
        content = try ContentID(reader.take(32).map { String(format: "%02x", $0) }.joined())
        guard let kind = ContentKind(rawValue: Int64(try reader.number(1))) else { throw ProtocolError.value }
        self.kind = kind; length = try reader.number(8); formatVersion = UInt32(try reader.number(4))
        logicalIdentity = try reader.take(16)
    }
}
public struct ReaderInventory: Equatable, Sendable {
    public let reader: Data
    public let generation: Data
    public let contents: [ContentManifest]
    public let complete: Bool
    public init(reader: Data, generation: Data, contents: [ContentManifest], complete: Bool) throws {
        guard [reader, generation].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }) else { throw ProtocolError.value }
        var seen: [ContentID: ContentManifest] = [:]; seen.reserveCapacity(contents.count)
        for entry in contents {
            if let previous = seen.updateValue(entry, forKey: entry.content), previous != entry { throw StoreError.invalidValue }
        }
        self.reader = reader; self.generation = generation; self.complete = complete
        self.contents = seen.values.sorted { $0.content.hex < $1.content.hex }
    }
}
public enum ContentReconciliationAction: Equatable, Sendable {
    case install(LibraryContent), remove(ContentManifest)
}
public enum ContentReconciliationError: Error, Equatable, Sendable { case incompleteInventory, wrongReader, conflictingManifest(ContentID) }

public extension LibraryStore {
    func reconcileContent(reader: Data, generation: Data, inventory: ReaderInventory) throws -> [ContentReconciliationAction] {
        guard reader == inventory.reader, generation == inventory.generation else { throw ContentReconciliationError.wrongReader }
        guard inventory.complete else { throw ContentReconciliationError.incompleteInventory }
        let selections = try readerSelections(reader: reader)
        var installed: [ContentID: ContentManifest] = [:]; installed.reserveCapacity(inventory.contents.count)
        for entry in inventory.contents { installed[entry.content] = entry }
        var actions: [ContentReconciliationAction] = []; actions.reserveCapacity(selections.count + inventory.contents.count)
        for selection in selections {
            guard let local = try content(selection.content) else { throw StoreError.missingContent }
            guard local.kind != .firmware else { throw StoreError.invalidValue }
            let desired = try selection.selected && !isLibraryContentDeleted(selection.content)
            if let remote = installed[selection.content] {
                guard remote.kind == local.kind, remote.length == local.length,
                      local.kind != .dictionary || (remote.formatVersion == 1 && remote.logicalIdentity == Data(count: 16)) else {
                    throw ContentReconciliationError.conflictingManifest(selection.content)
                }
                if !desired { actions.append(.remove(remote)) }
            } else if desired { actions.append(.install(local)) }
        }
        let chosen = Set(selections.map(\.content))
        for remote in inventory.contents where !chosen.contains(remote.content) {
            guard try isLibraryContentDeleted(remote.content) else { continue }
            guard let local = try content(remote.content), local.kind != .firmware else { continue }
            guard remote.kind == local.kind, remote.length == local.length,
                  local.kind != .dictionary || (remote.formatVersion == 1 && remote.logicalIdentity == Data(count: 16)) else {
                throw ContentReconciliationError.conflictingManifest(remote.content)
            }
            actions.append(.remove(remote))
        }
        return actions
    }
}

public enum ContentWork: Equatable, Sendable {
    case install(LibraryContent), remove(ContentManifest)
    case resume(TransferJob), abort(TransferJob), recoverCommit(TransferJob)
    case inspect(TransferJob), staleGeneration(TransferJob)
}
public extension LibraryStore {
    func reconcileContentWork(reader: Data, generation: Data, installation: Data,
                              inventory: ReaderInventory) throws -> [ContentWork] {
        guard installation.count == 16, installation.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
        let actions = try reconcileContent(reader: reader, generation: generation, inventory: inventory)
        let choices = try readerSelections(reader: reader)
        var selected: [ContentID: Bool] = [:]; selected.reserveCapacity(choices.count)
        for choice in choices { selected[choice.content] = choice.selected }
        let jobs = try pendingJobs().filter { $0.reader == reader }
        var active = Set<ContentID>(); active.reserveCapacity(jobs.count)
        var result: [ContentWork] = []; result.reserveCapacity(jobs.count + actions.count)
        for job in jobs {
            guard job.storageGeneration == generation else { result.append(.staleGeneration(job)); continue }
            guard active.insert(job.content).inserted else { throw StoreError.conflictingJob }
            if job.installation != installation {
                result.append(.inspect(job))
            } else if try hasTransferAbort(job.id) {
                result.append(.abort(job))
            } else if job.phase == .failed || selected[job.content] == nil {
                result.append(.inspect(job))
            } else if job.phase == .committing {
                result.append(.recoverCommit(job))
            } else if selected[job.content] == false {
                result.append(.abort(job))
            } else {
                result.append(.resume(job))
            }
        }
        for action in actions {
            switch action {
            case let .install(content): if !active.contains(content.id) { result.append(.install(content)) }
            case let .remove(manifest): if !active.contains(manifest.content) { result.append(.remove(manifest)) }
            }
        }
        return result
    }
}

public enum PreparedContentWork: Equatable, Sendable {
    case transfer(TransferJob), abort(TransferJob), remove(ContentManifest)
    case inspect(TransferJob), staleGeneration(TransferJob)
}
public extension LibraryStore {
    /// Persist new install jobs before the caller begins any reader operation.
    /// A failed preparation can leave reusable queued jobs, but never sends data.
    func prepareContentWork(reader: Data, generation: Data, installation: Data,
                            inventory: ReaderInventory) throws -> [PreparedContentWork] {
        let work = try reconcileContentWork(reader: reader, generation: generation,
                                           installation: installation, inventory: inventory)
        var prepared: [PreparedContentWork] = []; prepared.reserveCapacity(work.count)
        for item in work {
            switch item {
            case .install(let content):
                prepared.append(.transfer(try enqueueSelectedContent(content: content.id, reader: reader,
                    storageGeneration: generation, installation: installation)))
            case .resume(let job), .recoverCommit(let job): prepared.append(.transfer(job))
            case .abort(let job): prepared.append(.abort(job))
            case .remove(let manifest): prepared.append(.remove(manifest))
            case .inspect(let job): prepared.append(.inspect(job))
            case .staleGeneration(let job): prepared.append(.staleGeneration(job))
            }
        }
        return prepared
    }
}

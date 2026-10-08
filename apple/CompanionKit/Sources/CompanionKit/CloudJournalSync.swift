#if canImport(CloudKit)
import CloudKit
import Foundation

public enum CloudJournalSyncStatus: Equatable, Sendable {
    case disabled, ready, accountConfirmationRequired, failed(CloudSyncFailure)
}

public actor CloudJournalSync: CKSyncEngineDelegate {
    private let container: CKContainer
    private let vault: ContentVault
    private let importer: ContentImporter
    private let library: LibraryStore
    private let persistence: CloudSyncStateStore
    private let zone = CKRecordZone.ID(zoneName: CloudJournalRecord.zoneName)
    private var engine: CKSyncEngine?
    private var account: String?
    private var status: CloudJournalSyncStatus = .disabled
    private var synchronizing = false
    public init(containerIdentifier: String, library: LibraryStore, vault: ContentVault, persistence: CloudSyncStateStore) {
        container = CKContainer(identifier: containerIdentifier)
        self.library = library; self.persistence = persistence
        self.vault = vault; importer = ContentImporter(vault: vault, library: library)
    }
    public func currentStatus() -> CloudJournalSyncStatus { status }
    public func resumableAccount() async throws -> String? {
        let saved = await persistence.snapshot()
        guard saved.allowsForegroundResume(), let expected = saved.account else { return nil }
        let actual = try await observedAccount()
        guard actual == expected else { return nil }
        return actual
    }
    public func observedAccount() async throws -> String {
        let value = try await container.userRecordID().recordName
        if try await persistence.observeAccount(value) {
            engine = nil; account = nil; status = .accountConfirmationRequired
        }
        return value
    }
    // The UI must confirm the observed account before enabling a previously disabled store.
    public func start(confirmedAccount: String) async throws {
        let actual = try await observedAccount()
        guard actual == confirmedAccount else { throw CloudSyncStateError.accountConfirmationRequired }
        guard engine == nil else { return }
        try await persistence.enable(confirmedAccount: actual)
        let saved = await persistence.snapshot()
        let serialization = try saved.serialization.map { try JSONDecoder().decode(CKSyncEngine.State.Serialization.self, from: $0) }
        var configuration = CKSyncEngine.Configuration(database: container.privateCloudDatabase,
                                                       stateSerialization: serialization, delegate: self)
        configuration.automaticallySync = false
        account = actual; engine = CKSyncEngine(configuration); status = .ready
    }
    public func stop() async throws {
        engine = nil; account = nil; status = .disabled
        try await persistence.disable()
    }
    public func synchronize() async throws {
        guard !synchronizing else { throw StoreError.invalidTransition }
        synchronizing = true
        defer { synchronizing = false }
        guard let engine, let account, status == .ready else { throw CloudSyncStateError.accountConfirmationRequired }
        guard try await container.userRecordID().recordName == account else {
            try await stop(); throw CloudSyncStateError.accountConfirmationRequired
        }
        try await engine.fetchChanges()
        guard status == .ready else { throw CloudSyncStateError.invalidState }
        var visibilityCursor: UUID?
        while true {
            let page = try await library.libraryVisibilityChanges(after: visibilityCursor)
            guard !page.isEmpty else { break }
            let pending = try await library.pendingCloudVisibilityChanges(page, account: account)
            guard self.engine === engine, status == .ready else { throw CloudSyncStateError.invalidState }
            let pendingIDs = Set(pending.map(\.id))
            engine.state.remove(pendingRecordZoneChanges: page.filter { !pendingIDs.contains($0.id) }.map {
                .saveRecord(CKRecord.ID(recordName: "visibility-" + $0.id.uuidString, zoneID: zone))
            })
            if !pending.isEmpty {
                engine.state.add(pendingDatabaseChanges: [.saveZone(CKRecordZone(zoneID: zone))])
                engine.state.add(pendingRecordZoneChanges: pending.map {
                    .saveRecord(CKRecord.ID(recordName: "visibility-" + $0.id.uuidString, zoneID: zone))
                })
                try await engine.sendChanges()
            }
            guard status == .ready else { throw CloudSyncStateError.invalidState }
            visibilityCursor = page.last?.id
        }
        var contentCursor: ContentID?
        while true {
            let page = try await library.cloudContentPage(after: contentCursor)
            guard !page.isEmpty else { break }
            let pending = try await library.pendingCloudContent(page, account: account)
            guard self.engine === engine, status == .ready else { throw CloudSyncStateError.invalidState }
            let pendingIDs = Set(pending.map(\.id))
            engine.state.remove(pendingRecordZoneChanges: page.filter { !pendingIDs.contains($0.id) }.map {
                .saveRecord(CKRecord.ID(recordName: "content-" + $0.id.hex, zoneID: zone))
            })
            if !pending.isEmpty {
                engine.state.add(pendingDatabaseChanges: [.saveZone(CKRecordZone(zoneID: zone))])
                engine.state.add(pendingRecordZoneChanges: pending.map {
                    .saveRecord(CKRecord.ID(recordName: "content-" + $0.id.hex, zoneID: zone))
                })
                try await engine.sendChanges()
            }
            guard status == .ready else { throw CloudSyncStateError.invalidState }
            contentCursor = page.last?.id
        }
        var courseCursor: ContentID?
        while true {
            let page = try await library.cloudCourseAssociations(after: courseCursor)
            guard !page.isEmpty else { break }
            let pending = try await library.pendingCloudCourseAssociations(page, account: account)
            guard self.engine === engine, status == .ready else { throw CloudSyncStateError.invalidState }
            let pendingIDs = Set(pending.map(\.content))
            engine.state.remove(pendingRecordZoneChanges: page.filter { !pendingIDs.contains($0.content) }.map {
                .saveRecord(CKRecord.ID(recordName: "course-" + $0.content.hex, zoneID: zone))
            })
            if !pending.isEmpty {
                engine.state.add(pendingDatabaseChanges: [.saveZone(CKRecordZone(zoneID: zone))])
                engine.state.add(pendingRecordZoneChanges: pending.map {
                    .saveRecord(CKRecord.ID(recordName: "course-" + $0.content.hex, zoneID: zone))
                })
                try await engine.sendChanges()
            }
            guard status == .ready else { throw CloudSyncStateError.invalidState }
            courseCursor = page.last?.content
        }
        var cursor: EventIdentity?
        while true {
            let page = try await library.syncMutations(after: cursor, limit: 128)
            guard !page.isEmpty else { break }
            guard self.engine === engine, status == .ready else { throw CloudSyncStateError.invalidState }
            let pending = try await library.pendingCloudMutations(page, account: account)
            let pendingIdentities = Set(pending.map { $0.event.identity })
            engine.state.remove(pendingRecordZoneChanges: page.filter { !pendingIdentities.contains($0.event.identity) }.map {
                .saveRecord(CKRecord.ID(recordName: CloudJournalRecord($0).name, zoneID: zone))
            })
            if !pending.isEmpty {
                engine.state.add(pendingDatabaseChanges: [.saveZone(CKRecordZone(zoneID: zone))])
            }
            engine.state.add(pendingRecordZoneChanges: pending.map {
                .saveRecord(CKRecord.ID(recordName: CloudJournalRecord($0).name, zoneID: zone))
            })
            if !pending.isEmpty { try await engine.sendChanges() }
            guard status == .ready else { throw CloudSyncStateError.invalidState }
            cursor = page.last?.event.identity
        }
    }
    public func handleEvent(_ event: CKSyncEngine.Event, syncEngine: CKSyncEngine) async {
        guard engine === syncEngine, let account, status == .ready else { return }
        do {
            switch event {
            case .stateUpdate(let update):
                try await persistence.saveSerialization(JSONEncoder().encode(update.stateSerialization), account: account)
            case .accountChange:
                engine = nil; self.account = nil; status = .accountConfirmationRequired
                _ = try await persistence.observeAccount(nil)
            case .fetchedRecordZoneChanges(let fetched):
                // Journal removals cannot erase local causal history.
                guard fetched.deletions.isEmpty else { throw CloudSyncStateError.invalidState }
                let visibility = try fetched.modifications.filter { $0.record.recordType == "LilaVisibilityV1" }
                    .map { try Self.visibilityChange($0.record, zone: zone) }
                for offset in stride(from: 0, to: visibility.count, by: 250) {
                    let batch = Array(visibility[offset..<min(offset + 250, visibility.count)])
                    _ = try await library.importLibraryVisibilityChanges(batch)
                    try await library.acknowledgeCloudVisibilityChanges(batch, account: account)
                }
                var mutations: [JournalMutation] = []; mutations.reserveCapacity(fetched.modifications.count)
                for change in fetched.modifications {
                    if change.record.recordType == "LilaVisibilityV1" { continue }
                    if change.record.recordType == "LilaCourseV1" {
                        let association = try Self.courseAssociation(change.record, zone: zone)
                        try await library.acceptCloudCourseAssociation(association)
                        try await library.acknowledgeCloudCourseAssociation(association, account: account)
                    } else if change.record.recordType == "LilaContentV1" {
                        let descriptor = try await importContentRecord(change.record)
                        try await library.acknowledgeCloudContent([descriptor], account: account)
                    } else { mutations.append(try Self.decode(change.record, zone: zone).mutation) }
                }
                _ = try await library.importEvents(mutations)
                for offset in stride(from: 0, to: mutations.count, by: 250) {
                    try await library.acknowledgeCloudMutations(Array(mutations[offset..<min(offset + 250, mutations.count)]),
                                                               account: account)
                }
            case .fetchedDatabaseChanges(let fetched):
                guard !fetched.deletions.contains(where: { $0.zoneID == zone }) else { throw CloudSyncStateError.invalidState }
            case .sentRecordZoneChanges(let sent):
                var accepted: [JournalMutation] = []; accepted.reserveCapacity(sent.savedRecords.count)
                var contents: [CloudContentDescriptor] = []; contents.reserveCapacity(sent.savedRecords.count)
                for record in sent.savedRecords {
                    if record.recordType == "LilaVisibilityV1" {
                        try await library.acknowledgeCloudVisibilityChanges([Self.visibilityChange(record, zone: zone)], account: account)
                    } else if record.recordType == "LilaCourseV1" {
                        try await library.acknowledgeCloudCourseAssociation(Self.courseAssociation(record, zone: zone), account: account)
                    } else if record.recordType == "LilaContentV1" { contents.append(try Self.contentDescriptor(record, zone: zone)) }
                    else { accepted.append(try Self.decode(record, zone: zone).mutation) }
                }
                try await library.acknowledgeCloudContent(contents, account: account)
                try await library.acknowledgeCloudMutations(accepted, account: account)
                for failure in sent.failedRecordSaves {
                    if failure.error.code == .serverRecordChanged, let server = failure.error.serverRecord {
                        if server.recordType == "LilaVisibilityV1" {
                            let remote = try Self.visibilityChange(server, zone: zone)
                            let proposed = try Self.visibilityChange(failure.record, zone: zone)
                            guard remote == proposed else { throw LibraryVisibilityError.equivocation(proposed.id) }
                            try await library.acknowledgeCloudVisibilityChanges([remote], account: account)
                            syncEngine.state.remove(pendingRecordZoneChanges: [.saveRecord(server.recordID)])
                            continue
                        }
                        if server.recordType == "LilaCourseV1" {
                            let remote = try Self.courseAssociation(server, zone: zone)
                            let proposed = try Self.courseAssociation(failure.record, zone: zone)
                            guard remote == proposed else { throw StoreError.invalidValue }
                            try await library.acknowledgeCloudCourseAssociation(remote, account: account)
                            syncEngine.state.remove(pendingRecordZoneChanges: [.saveRecord(server.recordID)])
                            continue
                        }
                        if server.recordType == "LilaContentV1" {
                            let remote = try Self.contentDescriptor(server, zone: zone)
                            let proposed = try Self.contentDescriptor(failure.record, zone: zone)
                            guard remote.id == proposed.id, remote.kind == proposed.kind, remote.length == proposed.length else {
                                throw StoreError.invalidValue
                            }
                            let complete = try await container.privateCloudDatabase.record(for: server.recordID)
                            let verified = try await importContentRecord(complete)
                            try await library.acknowledgeCloudContent([verified], account: account)
                            syncEngine.state.remove(pendingRecordZoneChanges: [.saveRecord(server.recordID)])
                            continue
                        }
                        let remote = try Self.decode(server, zone: zone)
                        let proposed = try Self.decode(failure.record, zone: zone)
                        guard remote == proposed else { throw HistoryError.equivocation(proposed.mutation.event.identity) }
                        try await library.acknowledgeCloudMutations([remote.mutation], account: account)
                        syncEngine.state.remove(pendingRecordZoneChanges: [.saveRecord(server.recordID)])
                    } else { throw failure.error }
                }
            default: break
            }
        } catch {
            // Keep the previous durable cursor so rejected incoming records are fetched again after recovery.
            await reportFailure(error)
        }
    }
    public func nextRecordZoneChangeBatch(_ context: CKSyncEngine.SendChangesContext,
                                          syncEngine: CKSyncEngine) async -> CKSyncEngine.RecordZoneChangeBatch? {
        guard engine === syncEngine, status == .ready else { return nil }
        let changes = syncEngine.state.pendingRecordZoneChanges.filter { context.options.scope.contains($0) }
        return await CKSyncEngine.RecordZoneChangeBatch(pendingChanges: changes) { id in
            await self.outgoingRecord(id, syncEngine: syncEngine)
        }
    }
    private func outgoingRecord(_ id: CKRecord.ID, syncEngine: CKSyncEngine) async -> CKRecord? {
        guard engine === syncEngine, status == .ready else { return nil }
        do {
            if id.recordName.hasPrefix("visibility-") {
                guard id.zoneID == zone, let uuid = UUID(uuidString: String(id.recordName.dropFirst(11))),
                      let change = try await library.storedLibraryVisibilityChange(uuid),
                      id.recordName == "visibility-" + change.id.uuidString else { throw StoreError.invalidValue }
                guard engine === syncEngine, status == .ready else { return nil }
                let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
                let record = CKRecord(recordType: "LilaVisibilityV1", recordID: id)
                record["change"] = try encoder.encode(change) as CKRecordValue
                return record
            }
            if id.recordName.hasPrefix("course-") {
                let content = try ContentID(String(id.recordName.dropFirst(7)))
                guard id.zoneID == zone, let identity = try await library.courseIdentity(content),
                      !(try await library.isLibraryContentDeleted(content)) else { throw StoreError.invalidValue }
                let association = try CloudCourseAssociation(content: content, identity: identity)
                guard engine === syncEngine, status == .ready else { return nil }
                let record = CKRecord(recordType: "LilaCourseV1", recordID: id)
                record["association"] = try JSONEncoder().encode(association) as CKRecordValue
                return record
            }
            if id.recordName.hasPrefix("content-") {
                let contentID = try ContentID(String(id.recordName.dropFirst(8)))
                guard id.zoneID == zone, let content = try await library.content(contentID),
                      !(try await library.isLibraryContentDeleted(contentID)) else { throw StoreError.invalidValue }
                let descriptor = try CloudContentDescriptor(content: content)
                let object = try await vault.verifiedObject(contentID)
                guard object.length == descriptor.length, engine === syncEngine, status == .ready else {
                    throw StoreError.invalidValue
                }
                let record = CKRecord(recordType: "LilaContentV1", recordID: id)
                record["descriptor"] = try JSONEncoder().encode(descriptor) as CKRecordValue
                record["asset"] = CKAsset(fileURL: object.url)
                return record
            }
            guard id.zoneID == zone, let identity = Self.identity(id.recordName),
                  let mutation = try await library.storedEvent(identity) else { throw StoreError.invalidValue }
            guard engine === syncEngine, status == .ready else { return nil }
            let record = CKRecord(recordType: CloudJournalRecord.recordType, recordID: id)
            record["envelope"] = mutation.event.bytes as CKRecordValue
            record["body"] = mutation.body as CKRecordValue
            return record
        } catch {
            await reportFailure(error)
            return nil
        }
    }
    private static func visibilityChange(_ record: CKRecord, zone: CKRecordZone.ID) throws -> LibraryVisibilityChange {
        guard record.recordID.zoneID == zone, record.recordType == "LilaVisibilityV1",
              let bytes = record["change"] as? Data, bytes.count <= 4096 else { throw StoreError.invalidValue }
        let change = try JSONDecoder().decode(LibraryVisibilityChange.self, from: bytes)
        try change.validate()
        guard record.recordID.recordName == "visibility-" + change.id.uuidString else { throw StoreError.invalidValue }
        return change
    }
    private static func courseAssociation(_ record: CKRecord, zone: CKRecordZone.ID) throws -> CloudCourseAssociation {
        guard record.recordID.zoneID == zone, record.recordType == "LilaCourseV1",
              let bytes = record["association"] as? Data, bytes.count <= 1024 else { throw StoreError.invalidValue }
        let association = try JSONDecoder().decode(CloudCourseAssociation.self, from: bytes)
        try association.validate()
        guard record.recordID.recordName == "course-" + association.content.hex else { throw StoreError.invalidValue }
        return association
    }
    private static func contentDescriptor(_ record: CKRecord, zone: CKRecordZone.ID) throws -> CloudContentDescriptor {
        guard record.recordID.zoneID == zone, record.recordType == "LilaContentV1",
              let data = record["descriptor"] as? Data, data.count <= 4096 else { throw StoreError.invalidValue }
        let descriptor = try JSONDecoder().decode(CloudContentDescriptor.self, from: data)
        try descriptor.validate()
        guard record.recordID.recordName == "content-" + descriptor.id.hex else { throw StoreError.invalidValue }
        return descriptor
    }
    private func importContentRecord(_ record: CKRecord) async throws -> CloudContentDescriptor {
        let descriptor = try Self.contentDescriptor(record, zone: zone)
        guard let asset = record["asset"] as? CKAsset, let url = asset.fileURL else { throw StoreError.invalidValue }
        _ = try await importer.importCloudAsset(url, descriptor: descriptor)
        return descriptor
    }
    private static func failure(_ error: Error) -> CloudSyncFailure {
        guard let cloud = error as? CKError else { return .local(error) }
        switch cloud.code {
        case .quotaExceeded: return .quotaExceeded
        case .networkFailure, .networkUnavailable: return .networkUnavailable
        case .notAuthenticated, .accountTemporarilyUnavailable, .permissionFailure: return .accountUnavailable
        case .serviceUnavailable, .requestRateLimited, .zoneBusy: return .serviceUnavailable
        case .serverRecordChanged: return .journalConflict
        default: return .other
        }
    }
    public func reportFailure(_ error: Error) async {
        // Delegate failures take precedence over a subsequent generic operation error.
        if case .failed = status, error as? CloudSyncStateError == .invalidState { return }
        let persistedAccount = await persistence.snapshot().account
        let failedAccount = account ?? persistedAccount
        status = .failed(Self.failure(error)); engine = nil; account = nil
        do {
            if let cloud = error as? CKError, let failedAccount,
               [.networkFailure, .networkUnavailable, .serviceUnavailable, .requestRateLimited, .zoneBusy].contains(cloud.code) {
                let seconds = cloud.retryAfterSeconds ?? 30
                let delay = seconds.isFinite && seconds > 0 ? max(seconds, 1) : 30
                try await persistence.deferRetry(until: Date().addingTimeInterval(delay), account: failedAccount)
            } else { try await persistence.disable() }
        } catch { status = .failed(Self.failure(error)) }
    }
    private static func decode(_ record: CKRecord, zone: CKRecordZone.ID) throws -> CloudJournalRecord {
        guard record.recordID.zoneID == zone, record.recordType == CloudJournalRecord.recordType,
              let envelope = record["envelope"] as? Data, let body = record["body"] as? Data else { throw StoreError.invalidValue }
        return try CloudJournalRecord(name: record.recordID.recordName, envelope: envelope, body: body)
    }
    private static func identity(_ name: String) -> EventIdentity? {
        guard name.utf8.count == 64 else { return nil }
        var data = Data(); data.reserveCapacity(32)
        var offset = name.startIndex
        while offset < name.endIndex {
            let next = name.index(offset, offsetBy: 2)
            guard let byte = UInt8(name[offset..<next], radix: 16) else { return nil }
            data.append(byte); offset = next
        }
        var reader = ByteReader(data)
        return try? EventIdentity(origin: reader.take(16), epoch: reader.number(8), sequence: reader.number(8))
    }
}
#endif

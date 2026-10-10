import Foundation
import CSQLite
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif

public enum StoreError: Error, Equatable, Sendable {
    case database(String), invalidValue, unsupportedSchema, missingContent, missingJob, conflictingJob, invalidTransition, unresolvedPreferences
}

public struct ContentID: Hashable, Codable, Sendable {
    public let hex: String
    public init(_ hex: String) throws {
        guard hex.utf8.count == 64, hex.utf8.allSatisfy({ (48 ... 57).contains($0) || (97 ... 102).contains($0) }) else {
            throw StoreError.invalidValue
        }
        self.hex = hex
    }
    public var digest: Data {
        var bytes = Data(); bytes.reserveCapacity(32)
        var digits = hex.utf8.makeIterator()
        func value(_ digit: UInt8) -> UInt8 { digit <= 57 ? digit - 48 : digit - 87 }
        while let first = digits.next(), let second = digits.next() {
            bytes.append((value(first) << 4) | value(second))
        }
        return bytes
    }
    public init(from decoder: any Decoder) throws { try self.init(decoder.singleValueContainer().decode(String.self)) }
    public func encode(to encoder: any Encoder) throws { var container = encoder.singleValueContainer(); try container.encode(hex) }
}

public enum ContentKind: Int64, Codable, Equatable, Sendable { case epub = 1, course, font, dictionary, firmware }
public struct LibraryContent: Equatable, Sendable {
    public let id: ContentID
    public let kind: ContentKind
    public let length: UInt64
    public let title: String
    public let originalFilename: String
    public let authors: [String]
    public let identifiers: [String]
    public let languages: [String]
    public init(id: ContentID, kind: ContentKind, length: UInt64, title: String, originalFilename: String,
                authors: [String] = [], identifiers: [String] = [], languages: [String] = []) {
        self.id = id; self.kind = kind; self.length = length; self.title = title; self.originalFilename = originalFilename
        self.authors = authors; self.identifiers = identifiers; self.languages = languages
    }
}
public enum JobPhase: String, Equatable, Sendable { case queued, transferring, committing, paused, completed, failed, aborted }
public struct TransferJob: Equatable, Sendable {
    public let id: UUID
    public let reader: Data
    public let storageGeneration: Data
    public let installation: Data
    public let content: ContentID
    public let durableOffset: UInt64
    public let phase: JobPhase
}

public struct ReaderContentSelection: Equatable, Sendable {
    public let content: ContentID
    public let selected: Bool
}

public struct SavedReader: Equatable, Identifiable, Sendable {
    public var id: Data { device.identity }
    public let device: DeviceDescriptor
    public let lastConnected: Date
    public let lastSuccessfulSync: Date?
}

public struct ReaderJournalBaseline: Equatable, Sendable {
    public let reader: Data
    public let generation: Data
    public let frontier: Data
    public let count: UInt32
}

// One actor owns each SQLite handle; jobs contain no Keychain credentials.
public actor LibraryStore {
    private let database: Database
    private let random: @Sendable (Int) throws -> Data
    public init(url: URL, random: @escaping @Sendable (Int) throws -> Data = installationRandom) throws {
        self.random = random
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        database = try Database(url: url)
    }
    public func prepareReaderJournalMerge(owner: Data, previous: [JournalMutation], snapshot: JournalMergeSnapshot,
                                          inventory: ReaderInventory, transaction: UUID = UUID()) throws -> JournalMergeJob? {
        guard owner.count == 16, owner.contains(where: { $0 != 0 }), inventory.complete,
              transaction.uuidString != "00000000-0000-0000-0000-000000000000",
              previous.count == Int(snapshot.count),
              Set(previous.map(\.event.identity)).count == previous.count,
              try TintaJournalFrontier.digest(previous) == snapshot.frontier else { throw StoreError.invalidValue }
        for mutation in previous {
            guard try storedEvent(mutation.event.identity) == mutation else { throw StoreError.invalidValue }
        }
        if let pending = try pendingJournalMerge(reader: inventory.reader, generation: inventory.generation) {
            guard pending.declaration.owner == owner else { throw StoreError.conflictingJob }
            _ = try journalMergeMutations(pending)
            return pending
        }
        guard let baseline = try readerJournalBaseline(reader: inventory.reader, generation: inventory.generation),
              baseline.frontier == snapshot.frontier, baseline.count == snapshot.count else { throw HistoryError.staleFrontier }
        guard try pendingTintaMigration(reader: inventory.reader, generation: inventory.generation) == nil,
              try pendingTintaInstallation(reader: inventory.reader, generation: inventory.generation) == nil else {
            throw StoreError.conflictingJob
        }
        let known = try journalMutations()
        var indexed: [EventIdentity: JournalMutation] = [:]; indexed.reserveCapacity(known.count)
        for mutation in known { indexed[mutation.event.identity] = mutation }
        let prefix = Set(previous.map(\.event.identity))
        let books = Set(inventory.contents.filter { $0.kind == .epub }.map { $0.content.digest })
        let courses = Set(inventory.contents.filter { $0.kind == .course }.map(\.logicalIdentity))
        var selected = Set<EventIdentity>()
        var pending: [EventIdentity] = []; pending.reserveCapacity(known.count)
        for mutation in known {
            if mutation.event.kind == .preference ||
                (mutation.event.kind.rawValue < SyncEventKind.preference.rawValue && books.contains(mutation.event.resource)) {
                pending.append(mutation.event.identity)
            } else if mutation.event.kind.rawValue >= SyncEventKind.review.rawValue,
                      courses.contains(try TintaBody(mutation: mutation).subject.course) {
                pending.append(mutation.event.identity)
            }
        }
        while let identity = pending.popLast() {
            if prefix.contains(identity) || !selected.insert(identity).inserted { continue }
            guard let mutation = indexed[identity] else { throw HistoryError.missingAncestor(identity) }
            if mutation.event.kind.rawValue >= SyncEventKind.review.rawValue {
                let course = try TintaBody(mutation: mutation).subject.course
                guard courses.contains(course) else { throw JournalMergePlanningError.unavailableCourse(course) }
            }
            pending.append(contentsOf: mutation.event.ancestors)
            if identity.sequence > 1 {
                pending.append(try EventIdentity(origin: identity.origin, epoch: identity.epoch, sequence: identity.sequence - 1))
            }
        }
        if selected.isEmpty { return nil }
        let incoming = try selected.sorted().map { identity -> JournalMutation in
            guard let mutation = indexed[identity] else { throw HistoryError.missingAncestor(identity) }
            return mutation
        }
        guard previous.count + incoming.count <= Int(UInt32.max) else { throw ProtocolError.value }
        let declaration = try JournalMergeDeclaration(generation: inventory.generation,
            transaction: withUnsafeBytes(of: transaction.uuid) { Data($0) }, owner: owner, previous: snapshot,
            merged: JournalMergeSnapshot(count: UInt32(previous.count + incoming.count), recordSize: 1024,
                frontier: TintaJournalFrontier.digest(previous + incoming)))
        return try queueJournalMerge(reader: inventory.reader, declaration: declaration,
            previous: previous, incoming: incoming, inventory: inventory)
    }
    public func queueJournalMerge(reader: Data, declaration: JournalMergeDeclaration,
                                  previous: [JournalMutation], incoming: [JournalMutation],
                                  inventory: ReaderInventory) throws -> JournalMergeJob {
        guard reader.count == 16, reader.contains(where: { $0 != 0 }), inventory.complete,
              inventory.reader == reader, inventory.generation == declaration.generation,
              !incoming.isEmpty, previous.count == Int(declaration.previous.count),
              previous.count + incoming.count == Int(declaration.merged.count),
              Set((previous + incoming).map(\.event.identity)).count == previous.count + incoming.count,
              try TintaJournalFrontier.digest(previous) == declaration.previous.frontier,
              try TintaJournalFrontier.digest(previous + incoming) == declaration.merged.frontier else {
            throw StoreError.invalidValue
        }
        let preferences = try PreferenceHistory.reconcile(previous + incoming)
        guard !preferences.contains(where: {
            $0.key.rawValue >= PreferenceKey.tintaNewPerDay.rawValue && $0.requiresResolution
        }) else { throw StoreError.unresolvedPreferences }
        for preference in preferences where preference.key == .fontSelection || preference.key == .dictionary {
            let kind: ContentKind = preference.key == .fontSelection ? .font : .dictionary
            let installed = Set(inventory.contents.filter { $0.kind == kind }.map { $0.content.digest })
            if let missing = preference.missingContent(installed: installed) {
                throw JournalMergePlanningError.missingContent(missing.hash)
            }
        }
        let courses = inventory.contents.filter { $0.kind == .course }
        for mutation in incoming {
            try mutation.validateReaderBody()
            if mutation.event.kind.rawValue >= SyncEventKind.review.rawValue {
                let body = try TintaBody(mutation: mutation)
                guard courses.count == 1, courses[0].logicalIdentity == body.subject.course else {
                    throw StoreError.invalidValue
                }
                try requireCourseAssociation(resource: mutation.event.resource, course: body.subject.course)
            }
        }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        for mutation in previous + incoming {
            guard try storedEvent(mutation.event.identity) == mutation else { throw StoreError.invalidValue }
        }
        if let saved = try journalMergeJob(declaration.transaction) {
            guard saved.reader == reader, saved.declaration == declaration,
                  try journalMergeMutations(saved) == incoming else { throw StoreError.conflictingJob }
            try database.execute("COMMIT"); committed = true
            return saved
        }
        guard let baseline = try readerJournalBaseline(reader: reader, generation: declaration.generation),
              baseline.frontier == declaration.previous.frontier, baseline.count == declaration.previous.count else {
            throw HistoryError.staleFrontier
        }
        guard try pendingJournalMerge(reader: reader, generation: declaration.generation) == nil,
              try pendingTintaMigration(reader: reader, generation: declaration.generation) == nil,
              try pendingTintaInstallation(reader: reader, generation: declaration.generation) == nil else {
            throw StoreError.conflictingJob
        }
        try database.execute("INSERT INTO journal_merge_jobs(id,reader,generation,declaration,phase,acknowledged,paused) VALUES(?,?,?,?,'queued',?,0)",
            [.blob(declaration.transaction), .blob(reader), .blob(declaration.generation), .blob(declaration.bytes),
             .integer(Int64(declaration.previous.count))])
        for (index, mutation) in incoming.enumerated() {
            let payload = try JournalMergeRequest.append(transaction: declaration.transaction, mutation: mutation).payload()
            try database.execute("INSERT INTO journal_merge_events(job,ordinal,payload) VALUES(?,?,?)",
                [.blob(declaration.transaction), .integer(Int64(index)), .blob(payload)])
        }
        try database.execute("COMMIT"); committed = true
        return JournalMergeJob(reader: reader, declaration: declaration, phase: .queued,
                               acknowledgedCount: declaration.previous.count, paused: false)
    }
    public func journalMergeJob(_ transaction: Data) throws -> JournalMergeJob? {
        guard transaction.count == 16 else { throw StoreError.invalidValue }
        let query = try database.query("SELECT reader,generation,declaration,phase,acknowledged,paused,abort_state FROM journal_merge_jobs WHERE id=?", [.blob(transaction)])
        guard try query.next() else { return nil }
        let declaration = try JournalMergeDeclaration(decoding: query.blob(2))
        guard declaration.transaction == transaction, declaration.generation == query.blob(1),
              query.blob(0).count == 16, query.blob(0).contains(where: { $0 != 0 }),
              let phase = JournalMergeJobPhase(rawValue: query.text(3)),
              query.integer(4) >= Int64(declaration.previous.count), query.integer(4) <= Int64(declaration.merged.count),
              [0, 1].contains(query.integer(5)),
              let abortState = JournalMergeAbortState(rawValue: query.integer(6)),
              abortState == .none || phase != .completed,
              abortState != .requested || query.integer(5) == 1,
              abortState != .completed || query.integer(5) == 0,
              phase != .queued || query.integer(4) == Int64(declaration.previous.count),
              (phase != .committing && phase != .completed) || query.integer(4) == Int64(declaration.merged.count),
              phase != .completed || query.integer(5) == 0 else { throw StoreError.invalidValue }
        return JournalMergeJob(reader: query.blob(0), declaration: declaration, phase: phase,
                               acknowledgedCount: UInt32(query.integer(4)), paused: query.integer(5) == 1, abortState: abortState)
    }
    public func pendingJournalMerge(reader: Data, generation: Data) throws -> JournalMergeJob? {
        guard [reader, generation].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }) else {
            throw StoreError.invalidValue
        }
        let query = try database.query("SELECT id FROM journal_merge_jobs WHERE reader=? AND generation=? AND phase!='completed' AND abort_state!=2",
                                       [.blob(reader), .blob(generation)])
        guard try query.next() else { return nil }
        guard let job = try journalMergeJob(query.blob(0)) else { throw StoreError.invalidValue }
        return job
    }
    public func journalMergeMutations(_ job: JournalMergeJob) throws -> [JournalMutation] {
        guard try journalMergeJob(job.id) == job else { throw StoreError.conflictingJob }
        let query = try database.query("SELECT ordinal,payload FROM journal_merge_events WHERE job=? ORDER BY ordinal", [.blob(job.id)])
        var result: [JournalMutation] = []
        result.reserveCapacity(min(256, Int(job.declaration.merged.count - job.declaration.previous.count)))
        while try query.next() {
            guard query.integer(0) == Int64(result.count),
                  case let .append(transaction, mutation) = try JournalMergeRequest.decodePayload(query.blob(1)),
                  transaction == job.id, try storedEvent(mutation.event.identity) == mutation else { throw StoreError.invalidValue }
            result.append(mutation)
        }
        guard result.count == Int(job.declaration.merged.count - job.declaration.previous.count) else { throw StoreError.invalidValue }
        return result
    }
    @discardableResult
    public func updateJournalMerge(_ expected: JournalMergeJob, phase: JournalMergeJobPhase,
                                   acknowledgedCount: UInt32, paused: Bool = false) throws -> JournalMergeJob {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try journalMergeJob(expected.id) == expected else { throw StoreError.conflictingJob }
        guard expected.abortState == .none else { throw StoreError.invalidTransition }
        let same = phase == expected.phase
        let advances = (expected.phase == .queued && phase == .transferring) ||
            (expected.phase == .transferring && phase == .committing) ||
            (expected.phase == .committing && phase == .completed)
        guard same || advances, expected.phase != .completed || (same && !paused),
              acknowledgedCount >= expected.acknowledgedCount, acknowledgedCount <= expected.declaration.merged.count,
              phase != .queued || acknowledgedCount == expected.declaration.previous.count,
              (phase != .committing && phase != .completed) || acknowledgedCount == expected.declaration.merged.count else {
            throw StoreError.invalidTransition
        }
        if phase == .completed, expected.phase != .completed,
           let baseline = try readerJournalBaseline(reader: expected.reader, generation: expected.declaration.generation),
           baseline.frontier == expected.declaration.previous.frontier, baseline.count == expected.declaration.previous.count {
            try database.execute("UPDATE reader_journal_baselines SET frontier=?,event_count=? WHERE reader=? AND generation=?",
                [.blob(expected.declaration.merged.frontier), .integer(Int64(expected.declaration.merged.count)),
                 .blob(expected.reader), .blob(expected.declaration.generation)])
        }
        try database.execute("UPDATE journal_merge_jobs SET phase=?,acknowledged=?,paused=? WHERE id=?",
            [.text(phase.rawValue), .integer(Int64(acknowledgedCount)), .integer(paused ? 1 : 0), .blob(expected.id)])
        try database.execute("COMMIT"); committed = true
        return JournalMergeJob(reader: expected.reader, declaration: expected.declaration, phase: phase,
                               acknowledgedCount: acknowledgedCount, paused: paused)
    }
    public func requestJournalMergeAbort(_ expected: JournalMergeJob) throws -> JournalMergeJob {
        try setJournalMergeAbort(expected, completed: false)
    }
    public func completeJournalMergeAbort(_ expected: JournalMergeJob) throws -> JournalMergeJob {
        try setJournalMergeAbort(expected, completed: true)
    }
    // Caller verified the transaction's duplicate Begin and successful Commit acknowledgement.
    public func completeCommittedJournalMerge(_ expected: JournalMergeJob) throws -> JournalMergeJob {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try journalMergeJob(expected.id) == expected else { throw StoreError.conflictingJob }
        guard expected.phase == .committing, expected.abortState == .requested,
              expected.acknowledgedCount == expected.declaration.merged.count else { throw StoreError.invalidTransition }
        if let baseline = try readerJournalBaseline(reader: expected.reader, generation: expected.declaration.generation),
           baseline.frontier == expected.declaration.previous.frontier, baseline.count == expected.declaration.previous.count {
            try database.execute("UPDATE reader_journal_baselines SET frontier=?,event_count=? WHERE reader=? AND generation=?",
                [.blob(expected.declaration.merged.frontier), .integer(Int64(expected.declaration.merged.count)),
                 .blob(expected.reader), .blob(expected.declaration.generation)])
        }
        try database.execute("UPDATE journal_merge_jobs SET phase='completed',abort_state=0,paused=0 WHERE id=?", [.blob(expected.id)])
        try database.execute("COMMIT"); committed = true
        return JournalMergeJob(reader: expected.reader, declaration: expected.declaration, phase: .completed,
            acknowledgedCount: expected.acknowledgedCount, paused: false)
    }
    private func setJournalMergeAbort(_ expected: JournalMergeJob, completed: Bool) throws -> JournalMergeJob {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try journalMergeJob(expected.id) == expected else { throw StoreError.conflictingJob }
        guard expected.phase != .completed,
              !completed || expected.abortState != .none else { throw StoreError.invalidTransition }
        let state: JournalMergeAbortState = completed || expected.abortState == .completed ? .completed : .requested
        let paused = state == .requested
        if state != expected.abortState || paused != expected.paused {
            try database.execute("UPDATE journal_merge_jobs SET abort_state=?,paused=? WHERE id=?",
                [.integer(state.rawValue), .integer(paused ? 1 : 0), .blob(expected.id)])
        }
        try database.execute("COMMIT"); committed = true
        return JournalMergeJob(reader: expected.reader, declaration: expected.declaration, phase: expected.phase,
            acknowledgedCount: expected.acknowledgedCount, paused: paused, abortState: state)
    }
    public func prepareReaderTintaMigration(backup: ContentID, content: ContentID, owner: Data,
                                           previous: [JournalMutation], snapshot: JournalMergeSnapshot,
                                           inventory: ReaderInventory, vault: ContentVault,
                                           expectedDraft: LegacyMigrationDraft? = nil, transaction: UUID = UUID()) async throws -> TintaMigrationJob {
        try Task.checkCancellation()
        guard owner.count == 16, owner.contains(where: { $0 != 0 }),
              transaction.uuidString != "00000000-0000-0000-0000-000000000000", inventory.complete,
              previous.count == Int(snapshot.count), try TintaJournalFrontier.digest(previous) == snapshot.frontier else {
            throw StoreError.invalidValue
        }
        guard try !preferences().contains(where: {
            $0.key.rawValue >= PreferenceKey.tintaNewPerDay.rawValue && $0.requiresResolution
        }) else { throw StoreError.unresolvedPreferences }
        let manifest = try await vault.verifiedLegacyBackup(backup)
        guard manifest.reader == inventory.reader, manifest.generation == inventory.generation,
              manifest.course == (try courseIdentity(content)) else { throw ReaderSessionError.wrongReader }
        let pack = try courseManifest(content)
        guard pack.logicalIdentity == manifest.course,
              inventory.contents.filter({ $0.kind == .course }) == [pack] else { throw StoreError.invalidValue }
        guard let baseline = try readerJournalBaseline(reader: inventory.reader, generation: inventory.generation),
              baseline.frontier == snapshot.frontier, baseline.count == snapshot.count else { throw HistoryError.staleFrontier }
        for mutation in previous {
            guard try storedEvent(mutation.event.identity) == mutation else { throw StoreError.invalidValue }
        }
        guard try pendingTintaMigration(reader: inventory.reader, generation: inventory.generation) == nil,
              try pendingTintaInstallation(reader: inventory.reader, generation: inventory.generation) == nil,
              try pendingJournalMerge(reader: inventory.reader, generation: inventory.generation) == nil else {
            throw StoreError.conflictingJob
        }
        let backupTransaction: Data
        do {
            let query = try database.query("SELECT transaction_id FROM legacy_backup_jobs WHERE reader=? AND generation=? AND course=? AND backup=? AND completed=1",
                [.blob(manifest.reader), .blob(manifest.generation), .blob(manifest.course), .text(backup.hex)])
            guard try query.next() else { throw StoreError.missingJob }
            backupTransaction = query.blob(0)
        }
        let wire = try LegacyReaderBackupManifest(transaction: backupTransaction, manifest: manifest)
        if try sharedLegacyCanonical(backup) == nil {
            guard let expectedDraft, expectedDraft.backup == backup, expectedDraft.course == content,
                  expectedDraft.confirmedCourseIdentity == manifest.course else { throw StoreError.invalidValue }
            let origin: Data
            do {
                let reservation = try database.query("SELECT origin FROM legacy_migrations WHERE backup=?", [.text(backup.hex)])
                origin = try reservation.next() ? reservation.blob(0) : owner
            }
            let plan = try await prepareLegacyMigration(backup: backup, origin: origin, vault: vault, expectedDraft: expectedDraft)
            _ = try await installLegacyMigration(plan, backup: backup, vault: vault, expectedDraft: expectedDraft)
        } else {
            guard expectedDraft == nil else { throw StoreError.invalidValue }
        }
        try Task.checkCancellation()
        let known = try journalMutations()
        var indexed: [EventIdentity: JournalMutation] = [:]; indexed.reserveCapacity(known.count)
        for mutation in known { indexed[mutation.event.identity] = mutation }
        let prefix = Set(previous.map(\.event.identity))
        var selected = Set<EventIdentity>()
        var pending: [EventIdentity] = []; pending.reserveCapacity(known.count)
        for mutation in known {
            if mutation.event.kind == .preference {
                let preference = try PreferenceBody(decoding: mutation.body)
                if preference.key.rawValue >= PreferenceKey.tintaNewPerDay.rawValue { pending.append(mutation.event.identity) }
            } else if mutation.event.kind.rawValue >= SyncEventKind.review.rawValue,
                      try TintaBody(mutation: mutation).subject.course == manifest.course {
                pending.append(mutation.event.identity)
            }
        }
        while let identity = pending.popLast() {
            if prefix.contains(identity) || !selected.insert(identity).inserted { continue }
            guard let mutation = indexed[identity] else { throw HistoryError.missingAncestor(identity) }
            if mutation.event.kind.rawValue >= SyncEventKind.review.rawValue,
               try TintaBody(mutation: mutation).subject.course != manifest.course { throw StoreError.invalidValue }
            pending.append(contentsOf: mutation.event.ancestors)
            if identity.sequence > 1 {
                pending.append(try EventIdentity(origin: identity.origin, epoch: identity.epoch, sequence: identity.sequence - 1))
            }
        }
        let incoming = try selected.sorted().map { identity -> JournalMutation in
            guard let mutation = indexed[identity] else { throw HistoryError.missingAncestor(identity) }
            return mutation
        }
        guard previous.count + incoming.count <= Int(UInt32.max) else { throw ProtocolError.value }
        let merge = try JournalMergeDeclaration(generation: inventory.generation,
            transaction: withUnsafeBytes(of: transaction.uuid) { Data($0) }, owner: owner, previous: snapshot,
            merged: JournalMergeSnapshot(count: UInt32(previous.count + incoming.count), recordSize: 1024,
                frontier: TintaJournalFrontier.digest(previous + incoming)))
        let admission = try TintaMigrationAdmission(merge: merge, course: manifest.course, resource: content.digest,
            backupTransaction: backupTransaction, reader: inventory.reader, backupManifest: Data(SHA256.hash(data: wire.bytes)))
        return try await queueTintaMigration(backup: backup, admission: admission, previous: previous, incoming: incoming,
                                             inventory: inventory, vault: vault)
    }
    public func latestTintaMigration(backup: ContentID, reader: Data, generation: Data) throws -> TintaMigrationJob? {
        guard [reader, generation].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }) else { throw StoreError.invalidValue }
        let query = try database.query("SELECT id FROM tinta_migration_jobs WHERE backup=? AND reader=? AND generation=? ORDER BY rowid DESC LIMIT 1",
                                       [.text(backup.hex), .blob(reader), .blob(generation)])
        guard try query.next() else { return nil }
        return try tintaMigrationJob(query.blob(0))
    }
    public func queueTintaMigration(backup: ContentID, admission: TintaMigrationAdmission,
                                    previous: [JournalMutation], incoming: [JournalMutation],
                                    inventory: ReaderInventory, vault: ContentVault) async throws -> TintaMigrationJob {
        let manifest = try await vault.verifiedLegacyBackup(backup)
        let wire = try LegacyReaderBackupManifest(transaction: admission.backupTransaction, manifest: manifest)
        guard manifest.reader == admission.reader, manifest.generation == admission.merge.generation,
              manifest.course == admission.course, Data(SHA256.hash(data: wire.bytes)) == admission.backupManifest,
              inventory.complete, inventory.reader == admission.reader, inventory.generation == admission.merge.generation,
              previous.count == Int(admission.merge.previous.count),
              previous.count + incoming.count == Int(admission.merge.merged.count),
              Set((previous + incoming).map(\.event.identity)).count == previous.count + incoming.count,
              try TintaJournalFrontier.digest(previous) == admission.merge.previous.frontier,
              try TintaJournalFrontier.digest(previous + incoming) == admission.merge.merged.frontier else {
            throw StoreError.invalidValue
        }
        guard try !PreferenceHistory.reconcile(previous + incoming).contains(where: {
            $0.key.rawValue >= PreferenceKey.tintaNewPerDay.rawValue && $0.requiresResolution
        }) else { throw StoreError.unresolvedPreferences }
        for mutation in incoming {
            try mutation.validateReaderBody()
            if mutation.event.kind.rawValue >= SyncEventKind.review.rawValue {
                let body = try TintaBody(mutation: mutation)
                guard body.subject.course == admission.course else {
                    throw StoreError.invalidValue
                }
                try requireCourseAssociation(resource: mutation.event.resource, course: admission.course)
            }
        }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let packID = try ContentID(admission.resource.map { String(format: "%02x", $0) }.joined())
        let pack = try courseManifest(packID)
        guard pack.logicalIdentity == admission.course, inventory.contents.filter({ $0.kind == .course }) == [pack] else {
            throw StoreError.invalidValue
        }
        let registered = try database.query("SELECT reader,generation,course FROM legacy_backups WHERE hash=?", [.text(backup.hex)])
        guard try registered.next(), registered.blob(0) == manifest.reader, registered.blob(1) == manifest.generation,
              registered.blob(2) == manifest.course else { throw StoreError.invalidValue }
        let canonical = try sharedLegacyCanonical(backup) ?? backup
        let installed = try database.query("SELECT 1 FROM legacy_installations WHERE backup=?", [.text(canonical.hex)])
        guard try installed.next() else { throw StoreError.invalidValue }
        for mutation in previous + incoming {
            guard try storedEvent(mutation.event.identity) == mutation else { throw StoreError.invalidValue }
        }
        if let saved = try tintaMigrationJob(admission.merge.transaction) {
            guard saved.backup == backup, saved.admission == admission,
                  try tintaMigrationMutations(saved) == incoming else { throw StoreError.conflictingJob }
            try database.execute("COMMIT"); committed = true
            return saved
        }
        guard let baseline = try readerJournalBaseline(reader: admission.reader, generation: admission.merge.generation),
              baseline.frontier == admission.merge.previous.frontier, baseline.count == admission.merge.previous.count else {
            throw HistoryError.staleFrontier
        }
        let pending = try database.query("SELECT 1 FROM tinta_migration_jobs WHERE reader=? AND generation=? AND phase!='completed' AND abort_state!=2",
                                        [.blob(admission.reader), .blob(admission.merge.generation)])
        guard try !pending.next() else { throw StoreError.conflictingJob }
        guard try pendingTintaInstallation(reader: admission.reader, generation: admission.merge.generation) == nil,
              try pendingJournalMerge(reader: admission.reader, generation: admission.merge.generation) == nil else {
            throw StoreError.conflictingJob
        }
        try database.execute("""
            INSERT INTO tinta_migration_jobs(id,backup,reader,generation,admission,phase,acknowledged,paused)
            VALUES(?,?,?,?,?,'queued',?,0)
            """, [.blob(admission.merge.transaction), .text(backup.hex), .blob(admission.reader),
                    .blob(admission.merge.generation), .blob(admission.bytes), .integer(Int64(admission.merge.previous.count))])
        for (index, mutation) in incoming.enumerated() {
            let payload = try JournalMergeRequest.append(transaction: admission.merge.transaction, mutation: mutation).payload()
            try database.execute("INSERT INTO tinta_migration_events(job,ordinal,payload) VALUES(?,?,?)",
                                 [.blob(admission.merge.transaction), .integer(Int64(index)), .blob(payload)])
        }
        try database.execute("COMMIT"); committed = true
        return TintaMigrationJob(backup: backup, admission: admission, phase: .queued,
                                 acknowledgedCount: admission.merge.previous.count, paused: false)
    }
    public func tintaMigrationJob(_ transaction: Data) throws -> TintaMigrationJob? {
        guard transaction.count == 16 else { throw StoreError.invalidValue }
        let query = try database.query("SELECT backup,reader,generation,admission,phase,acknowledged,paused,abort_state FROM tinta_migration_jobs WHERE id=?",
                                       [.blob(transaction)])
        guard try query.next() else { return nil }
        let admission = try TintaMigrationAdmission(decoding: query.blob(3))
        guard admission.merge.transaction == transaction, admission.reader == query.blob(1),
              admission.merge.generation == query.blob(2), let phase = TintaMigrationJobPhase(rawValue: query.text(4)),
              query.integer(5) >= Int64(admission.merge.previous.count), query.integer(5) <= Int64(admission.merge.merged.count),
              [0, 1].contains(query.integer(6)),
              let abortState = TintaMigrationAbortState(rawValue: query.integer(7)),
              abortState == .none || phase != .completed,
              abortState != .requested || query.integer(6) == 1,
              abortState != .completed || query.integer(6) == 0 else { throw StoreError.invalidValue }
        let count = UInt32(query.integer(5))
        guard ((phase != .committing && phase != .completed) || count == admission.merge.merged.count),
              ((phase != .queued && phase != .admitting) || count == admission.merge.previous.count),
              phase != .completed || query.integer(6) == 0 else {
            throw StoreError.invalidValue
        }
        return TintaMigrationJob(backup: try ContentID(query.text(0)), admission: admission, phase: phase,
                                 acknowledgedCount: count, paused: query.integer(6) != 0, abortState: abortState)
    }
    public func pendingTintaMigration(reader: Data, generation: Data) throws -> TintaMigrationJob? {
        guard [reader, generation].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }) else {
            throw StoreError.invalidValue
        }
        let query = try database.query("SELECT id FROM tinta_migration_jobs WHERE reader=? AND generation=? AND phase!='completed' AND abort_state!=2",
                                       [.blob(reader), .blob(generation)])
        guard try query.next() else { return nil }
        guard let job = try tintaMigrationJob(query.blob(0)) else { throw StoreError.invalidValue }
        return job
    }
    public func tintaMigrationMutations(_ job: TintaMigrationJob) throws -> [JournalMutation] {
        guard try tintaMigrationJob(job.id) == job else { throw StoreError.conflictingJob }
        let query = try database.query("SELECT ordinal,payload FROM tinta_migration_events WHERE job=? ORDER BY ordinal", [.blob(job.id)])
        var result: [JournalMutation] = []
        result.reserveCapacity(min(256, Int(job.admission.merge.merged.count - job.admission.merge.previous.count)))
        while try query.next() {
            guard query.integer(0) == Int64(result.count),
                  case let .append(transaction, mutation) = try JournalMergeRequest.decodePayload(query.blob(1)),
                  transaction == job.id, try storedEvent(mutation.event.identity) == mutation else {
                throw StoreError.invalidValue
            }
            result.append(mutation)
        }
        guard result.count == Int(job.admission.merge.merged.count - job.admission.merge.previous.count) else {
            throw StoreError.invalidValue
        }
        return result
    }
    @discardableResult
    public func updateTintaMigration(_ expected: TintaMigrationJob, phase: TintaMigrationJobPhase,
                                     acknowledgedCount: UInt32, paused: Bool = false) throws -> TintaMigrationJob {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try tintaMigrationJob(expected.id) == expected else { throw StoreError.conflictingJob }
        guard expected.abortState == .none else { throw StoreError.invalidTransition }
        let same = phase == expected.phase
        let advances = (expected.phase == .queued && phase == .admitting) ||
            (expected.phase == .admitting && phase == .transferring) ||
            (expected.phase == .transferring && phase == .committing) ||
            (expected.phase == .committing && phase == .completed)
        guard same || advances, expected.phase != .completed || (same && !paused),
              acknowledgedCount >= expected.acknowledgedCount, acknowledgedCount <= expected.admission.merge.merged.count,
              (phase != .queued && phase != .admitting) || acknowledgedCount == expected.admission.merge.previous.count,
              (phase != .committing && phase != .completed) || acknowledgedCount == expected.admission.merge.merged.count else {
            throw StoreError.invalidTransition
        }
        if phase == .completed, expected.phase != .completed,
           let baseline = try readerJournalBaseline(reader: expected.admission.reader, generation: expected.admission.merge.generation),
           baseline.frontier == expected.admission.merge.previous.frontier, baseline.count == expected.admission.merge.previous.count {
            try database.execute("UPDATE reader_journal_baselines SET frontier=?,event_count=? WHERE reader=? AND generation=?",
                [.blob(expected.admission.merge.merged.frontier), .integer(Int64(expected.admission.merge.merged.count)),
                 .blob(expected.admission.reader), .blob(expected.admission.merge.generation)])
        }
        try database.execute("UPDATE tinta_migration_jobs SET phase=?,acknowledged=?,paused=? WHERE id=?",
                             [.text(phase.rawValue), .integer(Int64(acknowledgedCount)), .integer(paused ? 1 : 0), .blob(expected.id)])
        try database.execute("COMMIT"); committed = true
        return TintaMigrationJob(backup: expected.backup, admission: expected.admission, phase: phase,
                                 acknowledgedCount: acknowledgedCount, paused: paused)
    }
    public func requestTintaMigrationAbort(_ expected: TintaMigrationJob) throws -> TintaMigrationJob {
        try setTintaMigrationAbort(expected, completed: false)
    }
    public func completeTintaMigrationAbort(_ expected: TintaMigrationJob) throws -> TintaMigrationJob {
        try setTintaMigrationAbort(expected, completed: true)
    }
    // Caller verified the transaction's duplicate Begin and successful Commit acknowledgement.
    public func completeCommittedTintaMigration(_ expected: TintaMigrationJob) throws -> TintaMigrationJob {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try tintaMigrationJob(expected.id) == expected else { throw StoreError.conflictingJob }
        guard expected.phase == .committing, expected.abortState == .requested,
              expected.acknowledgedCount == expected.admission.merge.merged.count else { throw StoreError.invalidTransition }
        if let baseline = try readerJournalBaseline(reader: expected.admission.reader, generation: expected.admission.merge.generation),
           baseline.frontier == expected.admission.merge.previous.frontier, baseline.count == expected.admission.merge.previous.count {
            try database.execute("UPDATE reader_journal_baselines SET frontier=?,event_count=? WHERE reader=? AND generation=?",
                [.blob(expected.admission.merge.merged.frontier), .integer(Int64(expected.admission.merge.merged.count)),
                 .blob(expected.admission.reader), .blob(expected.admission.merge.generation)])
        }
        try database.execute("UPDATE tinta_migration_jobs SET phase='completed',abort_state=0,paused=0 WHERE id=?", [.blob(expected.id)])
        try database.execute("COMMIT"); committed = true
        return TintaMigrationJob(backup: expected.backup, admission: expected.admission, phase: .completed,
            acknowledgedCount: expected.acknowledgedCount, paused: false)
    }
    private func setTintaMigrationAbort(_ expected: TintaMigrationJob, completed: Bool) throws -> TintaMigrationJob {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try tintaMigrationJob(expected.id) == expected else { throw StoreError.conflictingJob }
        guard expected.phase != .completed,
              !completed || expected.abortState != .none else { throw StoreError.invalidTransition }
        let state: TintaMigrationAbortState = completed || expected.abortState == .completed ? .completed : .requested
        let paused = state == .requested
        if state != expected.abortState || paused != expected.paused {
            try database.execute("UPDATE tinta_migration_jobs SET abort_state=?,paused=? WHERE id=?",
                [.integer(state.rawValue), .integer(paused ? 1 : 0), .blob(expected.id)])
        }
        try database.execute("COMMIT"); committed = true
        return TintaMigrationJob(backup: expected.backup, admission: expected.admission, phase: expected.phase,
            acknowledgedCount: expected.acknowledgedCount, paused: paused, abortState: state)
    }
    public func queueRemoval(manifest: ContentManifest, inventory: ReaderInventory, installation: Data,
                             transaction: UUID = UUID()) throws -> ContentRemovalJob {
        guard inventory.complete, (manifest.kind == .epub || manifest.kind == .font || manifest.kind == .dictionary || manifest.kind == .course), inventory.contents.contains(manifest) else {
            throw StoreError.invalidValue
        }
        let request = try ContentRemovalRequest(transaction: withUnsafeBytes(of: transaction.uuid) { Data($0) },
            owner: installation, generation: inventory.generation, manifest: manifest)
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        if let saved = try removalJob(transaction) {
            guard saved.reader == inventory.reader, saved.request == request else { throw StoreError.conflictingJob }
            try database.execute("COMMIT"); committed = true; return saved
        }
        if try job(transaction) != nil { throw StoreError.conflictingJob }
        if try readerImportJob(transaction) != nil { throw StoreError.conflictingJob }
        let transfers = try database.query("""
            SELECT 1 FROM jobs WHERE reader=? AND generation=? AND content=?
            AND phase NOT IN ('completed','aborted') LIMIT 1
            """, [.blob(inventory.reader), .blob(inventory.generation), .text(manifest.content.hex)])
        guard try !transfers.next() else { throw StoreError.conflictingJob }
        guard try !hasPendingReaderImport(reader: inventory.reader, generation: inventory.generation,
                                         content: manifest.content) else { throw StoreError.conflictingJob }
        try database.execute("""
            INSERT INTO reader_selections(reader,content,selected)
            SELECT ?,hash,0 FROM content WHERE hash=?
            ON CONFLICT(reader,content) DO UPDATE SET selected=0 WHERE selected!=0
            """, [.blob(inventory.reader), .text(manifest.content.hex)])
        let pending = try database.query("SELECT id,reader,request,phase FROM removal_jobs WHERE reader=? AND phase!='completed'",
                                         [.blob(inventory.reader)])
        while try pending.next() {
            let saved = try decodeRemovalJob(pending)
            if saved.request.owner == request.owner, saved.request.generation == request.generation,
               saved.request.manifest == request.manifest {
                try database.execute("COMMIT"); committed = true; return saved
            }
        }
        try database.execute("INSERT INTO removal_jobs(id,reader,request,phase) VALUES(?,?,?,'queued')",
            [.text(transaction.uuidString), .blob(inventory.reader), .blob(request.encoded)])
        guard let created = try removalJob(transaction) else { throw StoreError.missingJob }
        try database.execute("COMMIT"); committed = true
        return created
    }
    public func enqueueReaderImport(manifest: ContentManifest, inventory: ReaderInventory, installation: Data,
                                    id: UUID = UUID()) throws -> ReaderImportJob {
        guard inventory.complete, inventory.contents.contains(manifest), manifest.length <= UInt64(Int64.max),
              installation.count == 16, installation.contains(where: { $0 != 0 }),
              withUnsafeBytes(of: id.uuid, { $0.contains(where: { $0 != 0 }) }) else { throw StoreError.invalidValue }
        _ = try ReaderContentReadRequest(generation: inventory.generation, manifest: manifest, offset: 0,
                                        maximumBytes: UInt16(ReaderContentReadRequest.maximumChunkBytes))
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        if let saved = try readerImportJob(id) {
            guard saved.reader == inventory.reader, saved.generation == inventory.generation,
                  saved.installation == installation, saved.manifest == manifest else { throw StoreError.conflictingJob }
            try database.execute("COMMIT"); committed = true; return saved
        }
        guard try job(id) == nil, try removalJob(id) == nil else { throw StoreError.conflictingJob }
        guard try !isReaderImportContentDeleted(manifest.content) else { throw StoreError.invalidTransition }
        let active = try database.query("""
            SELECT id,reader,generation,installation,manifest,acknowledged,phase,content FROM reader_import_jobs
            WHERE reader=? AND generation=? AND content=? AND phase NOT IN ('completed','aborted')
            """, [.blob(inventory.reader), .blob(inventory.generation), .text(manifest.content.hex)])
        if try active.next() {
            let saved = try decodeReaderImportJob(active)
            guard saved.installation == installation, saved.manifest == manifest else { throw StoreError.conflictingJob }
            try database.execute("COMMIT"); committed = true; return saved
        }
        guard try !hasPendingRemoval(reader: inventory.reader, generation: inventory.generation,
                                     content: manifest.content) else { throw StoreError.conflictingJob }
        try database.execute("""
            INSERT INTO reader_import_jobs(id,reader,generation,installation,manifest,content,acknowledged,phase)
            VALUES(?,?,?,?,?,?,0,'queued')
            """, [.text(id.uuidString), .blob(inventory.reader), .blob(inventory.generation), .blob(installation),
                  .blob(manifest.encoded), .text(manifest.content.hex)])
        guard let created = try readerImportJob(id) else { throw StoreError.missingJob }
        try database.execute("COMMIT"); committed = true; return created
    }
    public func readerImportJob(_ id: UUID) throws -> ReaderImportJob? {
        let query = try database.query("""
            SELECT id,reader,generation,installation,manifest,acknowledged,phase,content FROM reader_import_jobs WHERE id=?
            """, [.text(id.uuidString)])
        return try query.next() ? decodeReaderImportJob(query) : nil
    }
    public func bindReaderImportFilename(_ id: UUID, request: ReaderContentMetadataRequest,
                                         reply: ReaderContentMetadataReply) throws -> String {
        guard reply.result == .ok, reply.generation == request.generation, reply.manifest == request.manifest,
              let name = reply.originalFilename else { throw StoreError.invalidValue }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard let job = try readerImportJob(id) else { throw StoreError.missingJob }
        guard job.generation == request.generation, job.manifest == request.manifest else { throw StoreError.conflictingJob }
        guard job.phase != .aborted else { throw StoreError.invalidTransition }
        let saved = try database.query("SELECT original_filename FROM reader_import_filenames WHERE id=?", [.text(id.uuidString)])
        if try saved.next() {
            guard saved.text(0) == name else { throw StoreError.conflictingJob }
        } else {
            guard job.phase != .completed else { throw StoreError.invalidTransition }
            try database.execute("INSERT INTO reader_import_filenames(id,original_filename) VALUES(?,?)",
                                 [.text(id.uuidString), .text(name)])
        }
        try database.execute("COMMIT"); committed = true
        return name
    }
    public func readerImportFilename(_ id: UUID) throws -> String? {
        guard try readerImportJob(id) != nil else { throw StoreError.missingJob }
        let query = try database.query("SELECT original_filename FROM reader_import_filenames WHERE id=?", [.text(id.uuidString)])
        return try query.next() ? query.text(0) : nil
    }
    public func pendingReaderImports() throws -> [ReaderImportJob] {
        let query = try database.query("""
            SELECT id,reader,generation,installation,manifest,acknowledged,phase,content FROM reader_import_jobs
            WHERE phase NOT IN ('completed','aborted') ORDER BY id
            """)
        var jobs: [ReaderImportJob] = []; jobs.reserveCapacity(32)
        while try query.next() { jobs.append(try decodeReaderImportJob(query)) }
        return jobs
    }
    public func finishedReaderImports(after: UUID? = nil, limit: Int = 32) throws -> [ReaderImportJob] {
        guard (1...128).contains(limit) else { throw StoreError.invalidValue }
        let query = try database.query("""
            SELECT id,reader,generation,installation,manifest,acknowledged,phase,content FROM reader_import_jobs
            WHERE phase IN ('completed','aborted') AND id>? ORDER BY id LIMIT ?
            """, [.text(after?.uuidString ?? ""), .integer(Int64(limit))])
        var jobs: [ReaderImportJob] = []; jobs.reserveCapacity(limit)
        while try query.next() { jobs.append(try decodeReaderImportJob(query)) }
        return jobs
    }
    private func isReaderImportContentDeleted(_ id: ContentID) throws -> Bool {
        let query = try database.query("SELECT identity,content,payload FROM library_visibility_events WHERE content=?", [.text(id.hex)])
        var changes: [LibraryVisibilityChange] = []; changes.reserveCapacity(16)
        while try query.next() { changes.append(try decodeVisibility(query)) }
        return try LibraryVisibilityHistory.merge(changes, content: id,
            initiallyRemoved: isLibraryContentDeleted(id)).removed
    }
    private func abortReaderImportsInTransaction(content: ContentID) throws {
        try database.execute("""
            UPDATE reader_import_jobs SET phase='aborted' WHERE content=? AND phase NOT IN ('completed','aborted')
            """, [.text(content.hex)])
    }
    private func hasPendingReaderImport(reader: Data, generation: Data, content: ContentID) throws -> Bool {
        let query = try database.query("""
            SELECT 1 FROM reader_import_jobs WHERE reader=? AND generation=? AND content=?
            AND phase NOT IN ('completed','aborted') LIMIT 1
            """, [.blob(reader), .blob(generation), .text(content.hex)])
        return try query.next()
    }
    public func checkpointReaderImport(_ id: UUID, offset: UInt64, phase: ReaderImportJobPhase) throws {
        guard let saved = try readerImportJob(id) else { throw StoreError.missingJob }
        guard offset >= saved.acknowledgedOffset, offset <= saved.manifest.length,
              saved.phase != .completed, saved.phase != .aborted,
              phase != .completed, phase != .aborted else { throw StoreError.invalidTransition }
        if saved.phase == phase, saved.acknowledgedOffset == offset { return }
        let unchanged = offset == saved.acknowledgedOffset
        let permitted = (saved.phase == .queued && unchanged && (phase == .downloading || phase == .paused)) ||
            (saved.phase == .paused && unchanged && phase == .downloading) ||
            (saved.phase == .downloading && phase == .downloading) ||
            (unchanged && phase == .paused && (saved.phase == .downloading || saved.phase == .verifying)) ||
            (unchanged && offset == saved.manifest.length && phase == .verifying &&
             (saved.phase == .downloading || saved.phase == .paused))
        guard permitted else { throw StoreError.invalidTransition }
        try database.execute("UPDATE reader_import_jobs SET acknowledged=?,phase=? WHERE id=? AND phase=? AND acknowledged=?",
            [.integer(Int64(offset)), .text(phase.rawValue), .text(id.uuidString), .text(saved.phase.rawValue),
             .integer(Int64(saved.acknowledgedOffset))])
        guard database.changedRows == 1 else { throw StoreError.invalidTransition }
    }
    public func restartReaderImport(_ id: UUID) throws {
        guard let saved = try readerImportJob(id) else { throw StoreError.missingJob }
        guard saved.phase == .queued || saved.phase == .paused else { throw StoreError.invalidTransition }
        if saved.phase == .queued, saved.acknowledgedOffset == 0 { return }
        try database.execute("UPDATE reader_import_jobs SET acknowledged=0,phase='queued' WHERE id=? AND phase=? AND acknowledged=?",
            [.text(id.uuidString), .text(saved.phase.rawValue), .integer(Int64(saved.acknowledgedOffset))])
        guard database.changedRows == 1 else { throw StoreError.invalidTransition }
    }
    public func abortReaderImport(_ id: UUID) throws {
        guard let saved = try readerImportJob(id) else { throw StoreError.missingJob }
        guard saved.phase != .completed else { throw StoreError.invalidTransition }
        if saved.phase == .aborted { return }
        try database.execute("UPDATE reader_import_jobs SET phase='aborted' WHERE id=? AND phase=? AND acknowledged=?",
            [.text(id.uuidString), .text(saved.phase.rawValue), .integer(Int64(saved.acknowledgedOffset))])
        guard database.changedRows == 1 else { throw StoreError.invalidTransition }
    }
    // Called only after ContentImporter verifies and publishes the immutable object.
    func publishReaderImport(_ job: ReaderImportJob, content: LibraryContent,
                             courseMetadata: CoursePackMetadata? = nil) throws {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        if let bound = try readerImportFilename(job.id), bound != content.originalFilename {
            throw StoreError.conflictingJob
        }
        guard let saved = try readerImportJob(job.id) else { throw StoreError.missingJob }
        guard saved == job, saved.phase == .verifying, saved.acknowledgedOffset == saved.manifest.length,
              content.id == saved.manifest.content, content.kind == saved.manifest.kind,
              content.length == saved.manifest.length, try !isReaderImportContentDeleted(content.id),
              try !hasPendingRemoval(reader: saved.reader, generation: saved.generation, content: content.id) else {
            throw StoreError.invalidTransition
        }
        if let courseMetadata {
            guard content.kind == .course else { throw StoreError.invalidValue }
            try putCoursePackInTransaction(content, metadata: courseMetadata)
            try requireCourseAssociation(resource: content.id.digest, course: saved.manifest.logicalIdentity)
            try database.execute("INSERT INTO course_associations(content,identity) VALUES(?,?) ON CONFLICT(content) DO NOTHING",
                                 [.text(content.id.hex), .blob(saved.manifest.logicalIdentity)])
        } else {
            guard content.kind != .course, content.kind != .firmware else { throw StoreError.invalidValue }
            try putInTransaction(content)
        }
        try database.execute("""
            INSERT INTO reader_selections(reader,content,selected) VALUES(?,?,1)
            ON CONFLICT(reader,content) DO UPDATE SET selected=1 WHERE selected!=1
            """, [.blob(saved.reader), .text(content.id.hex)])
        try database.execute("UPDATE reader_import_jobs SET phase='completed' WHERE id=? AND phase='verifying' AND acknowledged=?",
                             [.text(saved.id.uuidString), .integer(Int64(saved.manifest.length))])
        guard database.changedRows == 1 else { throw StoreError.invalidTransition }
        try database.execute("COMMIT"); committed = true
    }
    private func decodeReaderImportJob(_ query: Statement) throws -> ReaderImportJob {
        guard let id = UUID(uuidString: query.text(0)), let phase = ReaderImportJobPhase(rawValue: query.text(6)),
              query.integer(5) >= 0 else { throw StoreError.invalidValue }
        let reader = query.blob(1), generation = query.blob(2), installation = query.blob(3)
        let manifest = try ContentManifest(decoding: query.blob(4)), offset = UInt64(query.integer(5))
        guard reader.count == 16, reader.contains(where: { $0 != 0 }),
              installation.count == 16, installation.contains(where: { $0 != 0 }),
              manifest.content.hex == query.text(7), manifest.length <= UInt64(Int64.max),
              offset <= manifest.length, (phase != .queued || offset == 0),
              (phase != .verifying && phase != .completed || offset == manifest.length),
              withUnsafeBytes(of: id.uuid, { $0.contains(where: { $0 != 0 }) }) else {
            throw StoreError.invalidValue
        }
        _ = try ReaderContentReadRequest(generation: generation, manifest: manifest, offset: 0,
                                        maximumBytes: UInt16(ReaderContentReadRequest.maximumChunkBytes))
        return ReaderImportJob(id: id, reader: reader, generation: generation, installation: installation,
                               manifest: manifest, acknowledgedOffset: offset, phase: phase)
    }
    public func removalJob(_ id: UUID) throws -> ContentRemovalJob? {
        let query = try database.query("SELECT id,reader,request,phase FROM removal_jobs WHERE id=?", [.text(id.uuidString)])
        return try query.next() ? decodeRemovalJob(query) : nil
    }
    public func pendingRemovalJobs() throws -> [ContentRemovalJob] {
        let query = try database.query("SELECT id,reader,request,phase FROM removal_jobs WHERE phase!='completed' ORDER BY id")
        var jobs: [ContentRemovalJob] = []; jobs.reserveCapacity(32)
        while try query.next() { jobs.append(try decodeRemovalJob(query)) }
        return jobs
    }
    private func hasPendingRemoval(reader: Data, generation: Data, content: ContentID) throws -> Bool {
        let query = try database.query("SELECT id,reader,request,phase FROM removal_jobs WHERE reader=? AND phase!='completed'",
                                       [.blob(reader)])
        while try query.next() {
            let saved = try decodeRemovalJob(query)
            if saved.request.generation == generation && saved.request.manifest.content == content { return true }
        }
        return false
    }
    public func checkpointRemoval(_ id: UUID, phase: ContentRemovalJobPhase) throws {
        guard let saved = try removalJob(id) else { throw StoreError.missingJob }
        if saved.phase == phase { return }
        let permitted = (saved.phase == .queued && (phase == .removing || phase == .paused)) ||
                        (saved.phase == .paused && phase == .removing) ||
                        (saved.phase == .removing && (phase == .paused || phase == .completed))
        guard permitted else { throw StoreError.invalidTransition }
        try database.execute("UPDATE removal_jobs SET phase=? WHERE id=? AND phase=?",
            [.text(phase.rawValue), .text(id.uuidString), .text(saved.phase.rawValue)])
        guard database.changedRows == 1 else { throw StoreError.invalidTransition }
    }
    private func decodeRemovalJob(_ query: Statement) throws -> ContentRemovalJob {
        guard let id = UUID(uuidString: query.text(0)), let phase = ContentRemovalJobPhase(rawValue: query.text(3)) else {
            throw StoreError.invalidValue
        }
        let reader = query.blob(1), request = try ContentRemovalRequest(decoding: query.blob(2))
        let transaction = withUnsafeBytes(of: id.uuid) { Data($0) }
        guard reader.count == 16, reader.contains(where: { $0 != 0 }),
              request.transaction == transaction else { throw StoreError.invalidValue }
        return ContentRemovalJob(id: id, reader: reader, request: request, phase: phase)
    }
    public func beginLegacyBackupExport(reader: Data, generation: Data, course: Data,
                                       previousTransaction: Data? = nil) throws -> Data {
        guard [reader, generation, course].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }) else {
            throw StoreError.invalidValue
        }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let values: [Binding] = [.blob(reader), .blob(generation), .blob(course)]
        let query = try database.query("SELECT transaction_id,completed FROM legacy_backup_jobs WHERE reader=? AND generation=? AND course=?", values)
        var old: Data?
        if try query.next() {
            old = query.blob(0)
            if query.integer(1) == 0 {
                let transaction = query.blob(0)
                guard transaction.count == 16, transaction.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
                try database.execute("COMMIT")
                committed = true
                return transaction
            }
        }
        let transaction = try previousTransaction ?? random(16)
        guard transaction.count == 16, transaction.contains(where: { $0 != 0 }), transaction != old else {
            throw StoreError.invalidValue
        }
        try database.execute("""
            INSERT INTO legacy_backup_jobs(reader,generation,course,transaction_id,completed,backup)
            VALUES(?,?,?,?,0,'') ON CONFLICT(reader,generation,course)
            DO UPDATE SET transaction_id=excluded.transaction_id,completed=0,backup=''
            """, values + [.blob(transaction)])
        try database.execute("COMMIT")
        committed = true
        return transaction
    }
    public func completedLegacyBackupExport(reader: Data, generation: Data, course: Data, transaction: Data,
                                           vault: ContentVault) async throws -> VerifiedLegacyBackup? {
        guard [reader, generation, course, transaction].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }) else {
            throw StoreError.invalidValue
        }
        let id: ContentID
        do {
            let query = try database.query("SELECT transaction_id,completed,backup FROM legacy_backup_jobs WHERE reader=? AND generation=? AND course=?",
                [.blob(reader), .blob(generation), .blob(course)])
            guard try query.next(), query.blob(0) == transaction else { throw StoreError.invalidValue }
            if query.integer(1) == 0 { return nil }
            id = try ContentID(query.text(2))
        }
        let manifest = try await vault.verifiedLegacyBackup(id)
        guard manifest.reader == reader, manifest.generation == generation, manifest.course == course else {
            throw StoreError.invalidValue
        }
        return VerifiedLegacyBackup(id: id, manifest: manifest)
    }
    public func completeLegacyBackupExport(reader: Data, generation: Data, course: Data,
                                          transaction: Data, backup: ContentID) throws {
        let values: [Binding] = [.blob(reader), .blob(generation), .blob(course)]
        let query = try database.query("SELECT transaction_id,completed,backup FROM legacy_backup_jobs WHERE reader=? AND generation=? AND course=?", values)
        guard try query.next(), query.blob(0) == transaction else { throw StoreError.invalidValue }
        if query.integer(1) == 1 {
            guard query.text(2) == backup.hex else { throw StoreError.invalidValue }
            return
        }
        throw StoreError.invalidValue
    }
    public func preserveLegacyBackupExport(_ export: LegacyReaderBackupManifest, sources: [LegacyBackupRole: URL],
                                          vault: ContentVault) async throws -> ContentID {
        let manifest = export.manifest
        let values: [Binding] = [.blob(manifest.reader), .blob(manifest.generation), .blob(manifest.course)]
        do {
            let pending = try database.query("SELECT transaction_id FROM legacy_backup_jobs WHERE reader=? AND generation=? AND course=?", values)
            guard try pending.next(), pending.blob(0) == export.transaction else { throw StoreError.invalidValue }
        }
        let object = try await vault.preserveLegacyBackup(manifest, sources: sources)
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let pending = try database.query("SELECT transaction_id,completed,backup FROM legacy_backup_jobs WHERE reader=? AND generation=? AND course=?", values)
        guard try pending.next(), pending.blob(0) == export.transaction else { throw StoreError.invalidValue }
        if pending.integer(1) == 1 {
            guard pending.text(2) == object.id.hex else { throw StoreError.invalidValue }
        } else {
            try database.execute("INSERT INTO legacy_backups(hash,reader,generation,course) VALUES(?,?,?,?) ON CONFLICT(hash) DO NOTHING",
                [.text(object.id.hex)] + values)
            let receipt = try database.query("SELECT reader,generation,course FROM legacy_backups WHERE hash=?", [.text(object.id.hex)])
            guard try receipt.next(), receipt.blob(0) == manifest.reader, receipt.blob(1) == manifest.generation,
                  receipt.blob(2) == manifest.course else { throw StoreError.invalidValue }
            try database.execute("UPDATE legacy_backup_jobs SET completed=1,backup=? WHERE reader=? AND generation=? AND course=? AND transaction_id=? AND completed=0",
                [.text(object.id.hex)] + values + [.blob(export.transaction)])
            guard database.changedRows == 1 else { throw StoreError.invalidValue }
        }
        try database.execute("COMMIT")
        committed = true
        return object.id
    }

    public func registerLegacyBackup(_ id: ContentID, vault: ContentVault) async throws {
        let manifest = try await vault.verifiedLegacyBackup(id)
        try database.execute("""
            INSERT INTO legacy_backups(hash,reader,generation,course) VALUES(?,?,?,?)
            ON CONFLICT(hash) DO NOTHING
            """, [.text(id.hex), .blob(manifest.reader), .blob(manifest.generation), .blob(manifest.course)])
        let query = try database.query("SELECT reader,generation,course FROM legacy_backups WHERE hash=?", [.text(id.hex)])
        guard try query.next(), query.blob(0) == manifest.reader, query.blob(1) == manifest.generation,
              query.blob(2) == manifest.course else { throw StoreError.invalidValue }
    }
    public func legacyBackupIDs(reader: Data, generation: Data, course: Data) throws -> [ContentID] {
        guard [reader, generation, course].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }) else {
            throw StoreError.invalidValue
        }
        let query = try database.query("SELECT hash FROM legacy_backups WHERE reader=? AND generation=? AND course=? ORDER BY hash",
                                       [.blob(reader), .blob(generation), .blob(course)])
        var result: [ContentID] = []; result.reserveCapacity(4)
        while try query.next() { result.append(try ContentID(query.text(0))) }
        return result
    }
    public func legacyOverlapCandidates(backup: ContentID, vault: ContentVault) async throws -> [LegacyHistoryOverlap] {
        let manifest = try await vault.verifiedLegacyBackup(backup)
        let query = try database.query("SELECT hash,reader,generation FROM legacy_backups WHERE course=? ORDER BY hash",
                                       [.blob(manifest.course)])
        var candidates: [ContentID] = []; candidates.reserveCapacity(4)
        var found = false
        while try query.next() {
            let id = try ContentID(query.text(0))
            if id == backup {
                guard query.blob(1) == manifest.reader, query.blob(2) == manifest.generation else { throw StoreError.invalidValue }
                found = true
            } else { candidates.append(id) }
        }
        guard found else { throw StoreError.invalidValue }
        var overlaps: [LegacyHistoryOverlap] = []; overlaps.reserveCapacity(candidates.count)
        for candidate in candidates {
            if let overlap = try await vault.legacyHistoryOverlap(first: backup, second: candidate) { overlaps.append(overlap) }
        }
        return overlaps
    }
    public func verifiedLegacyBackups(course: Data, vault: ContentVault) async throws -> [VerifiedLegacyBackup] {
        guard course.count == 16, course.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
        var registered: [(ContentID, Data, Data)] = []; registered.reserveCapacity(4)
        do {
            let query = try database.query("SELECT hash,reader,generation FROM legacy_backups WHERE course=? ORDER BY hash",
                                           [.blob(course)])
            while try query.next() {
                registered.append((try ContentID(query.text(0)), query.blob(1), query.blob(2)))
            }
        }
        var result: [VerifiedLegacyBackup] = []; result.reserveCapacity(registered.count)
        for (id, reader, generation) in registered {
            let manifest = try await vault.verifiedLegacyBackup(id)
            guard manifest.reader == reader, manifest.generation == generation, manifest.course == course else {
                throw StoreError.invalidValue
            }
            result.append(VerifiedLegacyBackup(id: id, manifest: manifest))
        }
        return result
    }
    public func preserveLegacyBackup(_ manifest: LegacyBackupManifest, sources: [LegacyBackupRole: URL],
                                     vault: ContentVault) async throws -> ContentID {
        let object = try await vault.preserveLegacyBackup(manifest, sources: sources)
        try await registerLegacyBackup(object.id, vault: vault)
        return object.id
    }
    @discardableResult
    public func confirmSharedLegacyBackup(_ backup: ContentID, canonical: ContentID, vault: ContentVault) async throws -> Bool {
        guard let overlap = try await vault.legacyHistoryOverlap(first: backup, second: canonical),
              overlap.evidence == .identicalLearnerFiles else { throw LegacySharedHistoryError.notIdentical }
        let source = try await vault.verifiedLegacyBackup(backup), target = try await vault.verifiedLegacyBackup(canonical)
        guard let reviews = target.files.first(where: { $0.role == .reviews }) else { throw StoreError.invalidValue }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        for (id, manifest) in [(backup, source), (canonical, target)] {
            let query = try database.query("SELECT reader,generation,course FROM legacy_backups WHERE hash=?", [.text(id.hex)])
            guard try query.next(), query.blob(0) == manifest.reader, query.blob(1) == manifest.generation,
                  query.blob(2) == manifest.course else { throw StoreError.invalidValue }
        }
        if let existing = try sharedLegacyCanonical(backup) {
            guard existing == canonical else { throw LegacySharedHistoryError.alreadyShared(existing) }
            try database.execute("COMMIT"); committed = true
            return false
        }
        let prepared = try database.query("SELECT 1 FROM legacy_migrations WHERE backup=? UNION SELECT 1 FROM legacy_preference_imports WHERE backup=?",
                                          [.text(backup.hex), .text(backup.hex)])
        guard try !prepared.next() else { throw LegacySharedHistoryError.alreadyPrepared }
        let digest = try validatedLegacyInstallationDigest(canonical, originalHash: reviews.id.digest)
        try database.execute("INSERT INTO legacy_shared_backups(backup,canonical,original_hash,digest) VALUES(?,?,?,?)",
            [.text(backup.hex), .text(canonical.hex), .blob(reviews.id.digest), .blob(digest)])
        try database.execute("COMMIT"); committed = true
        return true
    }
    public func sharedLegacyCanonical(_ backup: ContentID) throws -> ContentID? {
        let query = try database.query("SELECT canonical,original_hash,digest FROM legacy_shared_backups WHERE backup=?", [.text(backup.hex)])
        guard try query.next() else { return nil }
        let canonical = try ContentID(query.text(0))
        guard canonical != backup, try validatedLegacyInstallationDigest(canonical, originalHash: query.blob(1)) == query.blob(2) else {
            throw LegacySharedHistoryError.invalidReceipt
        }
        return canonical
    }
    private func validatedLegacyInstallationDigest(_ backup: ContentID, originalHash: Data) throws -> Data {
        guard originalHash.count == 32 else { throw LegacySharedHistoryError.invalidReceipt }
        let query = try database.query("""
            SELECT m.origin,m.epoch,m.resource,m.configuration,b.generation,b.course,i.digest
            FROM legacy_installations i JOIN legacy_migrations m ON m.backup=i.backup
            JOIN legacy_backups b ON b.hash=m.backup WHERE i.backup=?
            """, [.text(backup.hex)])
        guard try query.next() else { throw LegacySharedHistoryError.canonicalNotInstalled }
        let origin = query.blob(0), epoch = query.blob(1), resource = query.blob(2)
        guard origin.count == 16, epoch.count == 8 else { throw LegacySharedHistoryError.invalidReceipt }
        let generation = query.blob(4), course = query.blob(5), receipt = query.blob(6)
        let configurationHash = Data(SHA256.hash(data: query.blob(3)))
        let events = try database.query("SELECT identity,resource,envelope,body FROM sync_events WHERE substr(identity,1,24)=?",
                                        [.blob(origin + epoch)])
        var mutations: [JournalMutation] = []; mutations.reserveCapacity(256)
        while try events.next() { mutations.append(try decodeMutation(events)) }
        mutations.sort { $0.event.identity.sequence < $1.event.identity.sequence }
        var hasher = SHA256(); hasher.update(data: originalHash)
        for (index, mutation) in mutations.enumerated() {
            let event = mutation.event, body = try TintaBody(mutation: mutation)
            guard event.identity.sequence == UInt64(index) + 1, event.resource == resource,
                  event.storageGeneration == generation, body.subject.course == course,
                  event.kind != .review || event.schedulerConfiguration == configurationHash else {
                throw LegacySharedHistoryError.invalidReceipt
            }
            var lengths = Data(); lengths.reserveCapacity(8)
            lengths.appendLittleEndian(UInt64(event.bytes.count), count: 4)
            lengths.appendLittleEndian(UInt64(mutation.body.count), count: 4)
            hasher.update(data: lengths); hasher.update(data: event.bytes); hasher.update(data: mutation.body)
        }
        guard Data(hasher.finalize()) == receipt else { throw LegacySharedHistoryError.invalidReceipt }
        return receipt
    }
    func reserveLegacyMigration(backup: ContentID, origin: Data, resource: Data,
                                configuration: SchedulerConfiguration,
                                expectedBackups: Set<ContentID>? = nil) throws -> LegacyMigrationContext {
        guard origin.count == 16, origin.contains(where: { $0 != 0 }), resource.count == 32,
              resource.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        if let canonical = try sharedLegacyCanonical(backup) { throw LegacySharedHistoryError.alreadyShared(canonical) }
        let generation: Data
        do {
            let query = try database.query("SELECT generation,course FROM legacy_backups WHERE hash=?", [.text(backup.hex)])
            guard try query.next() else { throw StoreError.invalidValue }
            generation = query.blob(0)
            if let expectedBackups {
                guard try registeredLegacyBackups(course: query.blob(1)) == expectedBackups else {
                    throw LegacyMigrationError.staleBackupSet
                }
            }
            try requireCourseAssociation(resource: resource, course: query.blob(1))
        }
        let epoch: UInt64
        do {
            let query = try database.query("SELECT origin,epoch,resource,configuration FROM legacy_migrations WHERE backup=?", [.text(backup.hex)])
            if try query.next() {
                guard query.blob(0) == origin, query.blob(2) == resource, query.blob(3) == configuration.encoded else {
                    throw StoreError.invalidValue
                }
                let bytes = query.blob(1)
                guard bytes.count == 8 else { throw StoreError.invalidValue }
                var reader = ByteReader(bytes); epoch = try reader.number(8)
            } else {
                let bytes = try random(8)
                guard bytes.count == 8 else { throw CredentialError.invalidRandom }
                var reader = ByteReader(bytes); epoch = try reader.number(8)
                guard epoch > 0 else { throw CredentialError.invalidRandom }
                let collision = try database.query("""
                    SELECT 1 FROM local_origins WHERE origin=? AND epoch=?
                    UNION ALL SELECT 1 FROM legacy_migrations WHERE origin=? AND epoch=?
                    UNION ALL SELECT 1 FROM sync_events WHERE substr(identity,1,24)=? LIMIT 1
                    """, [.blob(origin), .blob(bytes), .blob(origin), .blob(bytes), .blob(origin + bytes)])
                guard try !collision.next() else { throw StoreError.invalidValue }
                try database.execute("INSERT INTO legacy_migrations(backup,origin,epoch,resource,configuration) VALUES(?,?,?,?,?)",
                    [.text(backup.hex), .blob(origin), .blob(bytes), .blob(resource), .blob(configuration.encoded)])
            }
        }
        let context = try LegacyMigrationContext(origin: origin, epoch: epoch, generation: generation,
                                                  resource: resource, configuration: configuration)
        try database.execute("COMMIT"); committed = true
        return context
    }
    private func registeredLegacyBackups(course: Data) throws -> Set<ContentID> {
        let query = try database.query("SELECT hash FROM legacy_backups WHERE course=?", [.blob(course)])
        var ids = Set<ContentID>(); ids.reserveCapacity(4)
        while try query.next() { ids.insert(try ContentID(query.text(0))) }
        return ids
    }
    public func prepareLegacyMigration(backup: ContentID, origin: Data, course: ContentID,
                                       confirmedCourseIdentity: Data, configuration: SchedulerConfiguration,
                                       vault: ContentVault,
                                       confirmedReadingResolutions: [UInt32: Set<UInt32>] = [:],
                                       confirmedIndependentBackups: Set<ContentID> = []) async throws -> LegacyMigrationPlan {
        try await prepareLegacyMigrationImpl(backup: backup, origin: origin, course: course,
            confirmedCourseIdentity: confirmedCourseIdentity, configuration: configuration, vault: vault,
            confirmedReadingResolutions: confirmedReadingResolutions, confirmedIndependentBackups: confirmedIndependentBackups,
            expectedBackups: nil)
    }
    private func prepareLegacyMigrationImpl(backup: ContentID, origin: Data, course: ContentID,
        confirmedCourseIdentity: Data, configuration: SchedulerConfiguration, vault: ContentVault,
        confirmedReadingResolutions: [UInt32: Set<UInt32>], confirmedIndependentBackups: Set<ContentID>,
        expectedBackups: Set<ContentID>?) async throws -> LegacyMigrationPlan {
        let manifest = try await vault.verifiedLegacyBackup(backup)
        if let canonical = try sharedLegacyCanonical(backup) { throw LegacySharedHistoryError.alreadyShared(canonical) }
        let cohort = try registeredLegacyBackups(course: manifest.course)
        let overlaps = try await legacyOverlapCandidates(backup: backup, vault: vault)
        let previouslyInstalled: Bool
        do {
            let installed = try database.query("SELECT 1 FROM legacy_installations WHERE backup=?", [.text(backup.hex)])
            previouslyInstalled = try installed.next()
        }
        if !previouslyInstalled {
            if let expectedBackups, expectedBackups != cohort { throw LegacyMigrationError.staleBackupSet }
            let candidates = Set(overlaps.map(\.secondBackup))
            guard confirmedIndependentBackups.isSubset(of: candidates) else { throw LegacyMigrationError.invalidOverlapDecision }
            let unresolved = overlaps.filter { !confirmedIndependentBackups.contains($0.secondBackup) }
            guard unresolved.isEmpty else { throw LegacyMigrationError.overlapConfirmationRequired(unresolved) }
        }
        let preview = try LegacyMigrationContext(origin: origin, epoch: 1, generation: manifest.generation,
            resource: course.digest, configuration: configuration)
        _ = try await vault.legacyMigrationPlan(backup: backup, context: preview, course: course,
            confirmedCourseIdentity: confirmedCourseIdentity, confirmedReadingResolutions: confirmedReadingResolutions)
        let reserved = try reserveLegacyMigration(backup: backup, origin: origin, resource: course.digest,
                                                  configuration: configuration, expectedBackups: cohort)
        return try await vault.legacyMigrationPlan(backup: backup, context: reserved, course: course,
            confirmedCourseIdentity: confirmedCourseIdentity, confirmedReadingResolutions: confirmedReadingResolutions)
    }
    public func legacyMigrationDraft(_ backup: ContentID) throws -> LegacyMigrationDraft? {
        let query = try database.query("SELECT payload FROM legacy_migration_drafts WHERE backup=?", [.text(backup.hex)])
        guard try query.next() else { return nil }
        let bytes = query.blob(0)
        guard bytes.count <= 1048576 else { throw StoreError.invalidValue }
        let draft = try JSONDecoder().decode(LegacyMigrationDraft.self, from: bytes)
        try draft.validate()
        guard draft.backup == backup else { throw StoreError.invalidValue }
        return draft
    }
    @discardableResult
    public func saveLegacyMigrationDraft(backup: ContentID, course: ContentID, confirmedCourseIdentity: Data,
        configuration: SchedulerConfiguration, vault: ContentVault,
        confirmedReadingResolutions: [UInt32: Set<UInt32>] = [:],
        confirmedIndependentBackups: Set<ContentID> = []) async throws -> Bool {
        let manifest = try await vault.verifiedLegacyBackup(backup)
        let cohort = try registeredLegacyBackups(course: manifest.course)
        let overlaps = try await legacyOverlapCandidates(backup: backup, vault: vault)
        guard confirmedIndependentBackups.isSubset(of: Set(overlaps.map(\.secondBackup))) else {
            throw LegacyMigrationError.invalidOverlapDecision
        }
        let unresolved = overlaps.filter { !confirmedIndependentBackups.contains($0.secondBackup) }
        guard unresolved.isEmpty else { throw LegacyMigrationError.overlapConfirmationRequired(unresolved) }
        let preview = try LegacyMigrationContext(origin: manifest.reader, epoch: 1, generation: manifest.generation,
            resource: course.digest, configuration: configuration)
        _ = try await vault.legacyMigrationPlan(backup: backup, context: preview, course: course,
            confirmedCourseIdentity: confirmedCourseIdentity, confirmedReadingResolutions: confirmedReadingResolutions)
        let draft = LegacyMigrationDraft(backup: backup, course: course, confirmedCourseIdentity: confirmedCourseIdentity,
            configuration: configuration, readings: confirmedReadingResolutions,
            independent: confirmedIndependentBackups, reviewed: cohort)
        try draft.validate()
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let payload = try encoder.encode(draft)
        guard payload.count <= 1048576 else { throw StoreError.invalidValue }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        if let canonical = try sharedLegacyCanonical(backup) { throw LegacySharedHistoryError.alreadyShared(canonical) }
        guard try registeredLegacyBackups(course: manifest.course) == cohort else { throw LegacyMigrationError.staleBackupSet }
        try requireCourseAssociation(resource: course.digest, course: confirmedCourseIdentity)
        if try legacyMigrationDraft(backup) == draft {
            try database.execute("COMMIT"); committed = true
            return false
        }
        let installed = try database.query("SELECT 1 FROM legacy_installations WHERE backup=?", [.text(backup.hex)])
        guard try !installed.next() else { throw LegacySharedHistoryError.alreadyPrepared }
        let reserved = try database.query("SELECT resource,configuration FROM legacy_migrations WHERE backup=?", [.text(backup.hex)])
        if try reserved.next() {
            guard reserved.blob(0) == course.digest, reserved.blob(1) == configuration.encoded else { throw StoreError.invalidValue }
        }
        try database.execute("""
            INSERT INTO legacy_migration_drafts(backup,payload) VALUES(?,?)
            ON CONFLICT(backup) DO UPDATE SET payload=excluded.payload
            """, [.text(backup.hex), .blob(payload)])
        try database.execute("COMMIT"); committed = true
        return true
    }
    public func prepareLegacyMigration(backup: ContentID, origin: Data, vault: ContentVault,
                                       expectedDraft: LegacyMigrationDraft? = nil) async throws -> LegacyMigrationPlan {
        guard let draft = try legacyMigrationDraft(backup) else { throw StoreError.invalidValue }
        guard expectedDraft == nil || expectedDraft == draft else { throw StoreError.invalidValue }
        let configuration = try SchedulerConfiguration(retentionBasisPoints: draft.retentionBasisPoints,
                                                       maximumInterval: draft.maximumInterval)
        return try await prepareLegacyMigrationImpl(backup: backup, origin: origin, course: draft.course,
            confirmedCourseIdentity: draft.confirmedCourseIdentity, configuration: configuration, vault: vault,
            confirmedReadingResolutions: draft.confirmedReadingResolutions,
            confirmedIndependentBackups: Set(draft.independentBackups), expectedBackups: Set(draft.reviewedBackups))
    }
    @discardableResult
    public func installLegacyMigration(_ plan: LegacyMigrationPlan, backup: ContentID,
                                       vault: ContentVault, expectedDraft: LegacyMigrationDraft? = nil) async throws -> Int {
        let manifest = try await vault.verifiedLegacyBackup(backup)
        guard let reviews = manifest.files.first(where: { $0.role == .reviews }),
              reviews.id.hex == plan.originalHash.map({ String(format: "%02x", $0) }).joined() else {
            throw StoreError.invalidValue
        }
        guard try TintaHistory.replay(plan.mutations) == plan.snapshot else { throw StoreError.invalidValue }
        var hasher = SHA256()
        hasher.update(data: plan.originalHash)
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        if let expectedDraft {
            guard expectedDraft.backup == backup, try legacyMigrationDraft(backup) == expectedDraft else {
                throw StoreError.invalidValue
            }
        }
        if let canonical = try sharedLegacyCanonical(backup) { throw LegacySharedHistoryError.alreadyShared(canonical) }
        let origin: Data, epoch: UInt64, resource: Data, configuration: Data
        do {
            let query = try database.query("""
                SELECT m.origin,m.epoch,m.resource,m.configuration,b.reader,b.generation,b.course
                FROM legacy_migrations m JOIN legacy_backups b ON b.hash=m.backup WHERE m.backup=?
                """, [.text(backup.hex)])
            guard try query.next(), query.blob(4) == manifest.reader, query.blob(5) == manifest.generation,
                  query.blob(6) == manifest.course else { throw StoreError.invalidValue }
            origin = query.blob(0); resource = query.blob(2); configuration = query.blob(3)
            let bytes = query.blob(1)
            guard bytes.count == 8 else { throw StoreError.invalidValue }
            var reader = ByteReader(bytes); epoch = try reader.number(8)
        }
        try requireCourseAssociation(resource: resource, course: manifest.course)
        let configurationHash = Data(SHA256.hash(data: configuration))
        for (index, mutation) in plan.mutations.enumerated() {
            let event = mutation.event, body = try TintaBody(mutation: mutation)
            guard event.identity.origin == origin, event.identity.epoch == epoch,
                  event.identity.sequence == UInt64(index) + 1, event.resource == resource,
                  event.storageGeneration == manifest.generation, body.subject.course == manifest.course else {
                throw StoreError.invalidValue
            }
            if event.kind == .review {
                guard event.schedulerConfiguration == configurationHash else { throw StoreError.invalidValue }
            }
            var lengths = Data(); lengths.reserveCapacity(8)
            lengths.appendLittleEndian(UInt64(event.bytes.count), count: 4)
            lengths.appendLittleEndian(UInt64(mutation.body.count), count: 4)
            hasher.update(data: lengths); hasher.update(data: event.bytes); hasher.update(data: mutation.body)
        }
        let digest = Data(hasher.finalize())
        do {
            let query = try database.query("SELECT digest FROM legacy_installations WHERE backup=?", [.text(backup.hex)])
            if try query.next() {
                guard query.blob(0) == digest else { throw StoreError.invalidValue }
                for mutation in plan.mutations {
                    guard try storedEvent(mutation.event.identity) == mutation else { throw StoreError.invalidValue }
                }
                try database.execute("COMMIT"); committed = true
                return 0
            }
        }
        let inserted = try importEventsInTransaction(plan.mutations)
        try database.execute("INSERT INTO legacy_installations(backup,digest) VALUES(?,?)", [.text(backup.hex), .blob(digest)])
        try database.execute("COMMIT"); committed = true
        return inserted
    }
    public func isLibraryContentDeleted(_ content: ContentID) throws -> Bool {
        let query = try database.query("SELECT 1 FROM library_deletions WHERE content=?", [.text(content.hex)])
        return try query.next()
    }
    public func libraryContentIDs() throws -> [ContentID] {
        let query = try database.query("""
            SELECT hash FROM content WHERE NOT EXISTS(SELECT 1 FROM library_deletions WHERE content=hash) ORDER BY hash
            """)
        var ids: [ContentID] = []; ids.reserveCapacity(64)
        while try query.next() { ids.append(try ContentID(query.text(0))) }
        return ids
    }
    public func cloudContentPage(after id: ContentID? = nil, limit: Int = 32) throws -> [LibraryContent] {
        guard (1...128).contains(limit) else { throw StoreError.invalidValue }
        let query = try database.query("""
            SELECT hash FROM content WHERE hash>? AND kind!=5
            AND NOT EXISTS(SELECT 1 FROM library_deletions WHERE content=hash) ORDER BY hash LIMIT ?
            """, [.text(id?.hex ?? ""), .integer(Int64(limit))])
        var result: [LibraryContent] = []; result.reserveCapacity(limit)
        while try query.next() {
            guard let content = try self.content(ContentID(query.text(0))) else { throw StoreError.invalidValue }
            result.append(content)
        }
        return result
    }
    public func pendingCloudContent(_ page: [LibraryContent], account: String) throws -> [LibraryContent] {
        guard page.count <= 128 else { throw StoreError.invalidValue }
        let key = try cloudAccountKey(account)
        var pending: [LibraryContent] = []; pending.reserveCapacity(page.count)
        for content in page {
            let query = try database.query("SELECT kind,length FROM cloud_content_receipts WHERE account=? AND content=?",
                                           [.blob(key), .text(content.id.hex)])
            if try query.next() {
                guard query.integer(0) == content.kind.rawValue, UInt64(query.integer(1)) == content.length else {
                    throw StoreError.invalidValue
                }
            } else { pending.append(content) }
        }
        return pending
    }
    public func acknowledgeCloudContent(_ descriptors: [CloudContentDescriptor], account: String) throws {
        guard descriptors.count <= 250 else { throw StoreError.invalidValue }
        let key = try cloudAccountKey(account)
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        for descriptor in descriptors {
            try descriptor.validate()
            guard let stored = try content(descriptor.id), stored.kind == descriptor.kind,
                  stored.length == descriptor.length else { throw StoreError.invalidValue }
            let receipt = try database.query("SELECT kind,length FROM cloud_content_receipts WHERE account=? AND content=?",
                                             [.blob(key), .text(descriptor.id.hex)])
            if try receipt.next() {
                guard receipt.integer(0) == descriptor.kind.rawValue, UInt64(receipt.integer(1)) == descriptor.length else {
                    throw StoreError.invalidValue
                }
            } else {
                try database.execute("INSERT INTO cloud_content_receipts(account,content,kind,length) VALUES(?,?,?,?)",
                                     [.blob(key), .text(descriptor.id.hex), .integer(descriptor.kind.rawValue), .integer(Int64(descriptor.length))])
            }
        }
        try database.execute("COMMIT"); committed = true
    }
    public func deletedLibraryContentIDs() throws -> [ContentID] {
        let query = try database.query("SELECT content FROM library_deletions ORDER BY content")
        var ids: [ContentID] = []; ids.reserveCapacity(32)
        while try query.next() { ids.append(try ContentID(query.text(0))) }
        return ids
    }
    public func libraryVisibilityChanges(after id: UUID? = nil, limit: Int = 128) throws -> [LibraryVisibilityChange] {
        guard (1...250).contains(limit) else { throw StoreError.invalidValue }
        let query = try database.query("SELECT identity,content,payload FROM library_visibility_events WHERE identity>? ORDER BY identity LIMIT ?",
                                       [.text(id?.uuidString ?? ""), .integer(Int64(limit))])
        var result: [LibraryVisibilityChange] = []; result.reserveCapacity(limit)
        while try query.next() { result.append(try decodeVisibility(query)) }
        return result
    }
    private func visibilityFingerprint(_ change: LibraryVisibilityChange) throws -> Data {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        return Data(SHA256.hash(data: try encoder.encode(change)))
    }
    public func pendingCloudVisibilityChanges(_ page: [LibraryVisibilityChange], account: String) throws -> [LibraryVisibilityChange] {
        guard page.count <= 250 else { throw StoreError.invalidValue }
        let key = try cloudAccountKey(account)
        var result: [LibraryVisibilityChange] = []; result.reserveCapacity(page.count)
        for change in page {
            try change.validate()
            let query = try database.query("SELECT fingerprint FROM cloud_visibility_receipts WHERE account=? AND identity=?",
                                           [.blob(key), .text(change.id.uuidString)])
            if try query.next() {
                guard try query.blob(0) == visibilityFingerprint(change) else { throw LibraryVisibilityError.equivocation(change.id) }
            } else { result.append(change) }
        }
        return result
    }
    public func acknowledgeCloudVisibilityChanges(_ changes: [LibraryVisibilityChange], account: String) throws {
        guard changes.count <= 250 else { throw StoreError.invalidValue }
        let key = try cloudAccountKey(account)
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        for change in changes {
            try change.validate()
            let event = try database.query("SELECT identity,content,payload FROM library_visibility_events WHERE identity=?",
                                           [.text(change.id.uuidString)])
            guard try event.next(), try decodeVisibility(event) == change else { throw StoreError.invalidValue }
            let fingerprint = try visibilityFingerprint(change)
            let receipt = try database.query("SELECT fingerprint FROM cloud_visibility_receipts WHERE account=? AND identity=?",
                                             [.blob(key), .text(change.id.uuidString)])
            if try receipt.next() {
                guard receipt.blob(0) == fingerprint else { throw LibraryVisibilityError.equivocation(change.id) }
            } else {
                try database.execute("INSERT INTO cloud_visibility_receipts(account,identity,fingerprint) VALUES(?,?,?)",
                                     [.blob(key), .text(change.id.uuidString), .blob(fingerprint)])
            }
        }
        try database.execute("COMMIT"); committed = true
    }
    public func storedLibraryVisibilityChange(_ id: UUID) throws -> LibraryVisibilityChange? {
        let query = try database.query("SELECT identity,content,payload FROM library_visibility_events WHERE identity=?", [.text(id.uuidString)])
        return try query.next() ? decodeVisibility(query) : nil
    }
    private func decodeVisibility(_ query: Statement) throws -> LibraryVisibilityChange {
        let bytes = query.blob(2)
        guard bytes.count <= 4096 else { throw StoreError.invalidValue }
        let change = try JSONDecoder().decode(LibraryVisibilityChange.self, from: bytes)
        try change.validate()
        guard change.id.uuidString == query.text(0), change.content.hex == query.text(1) else { throw StoreError.invalidValue }
        return change
    }
    @discardableResult
    public func importLibraryVisibilityChanges(_ incoming: [LibraryVisibilityChange]) throws -> Int {
        guard incoming.count <= 250 else { throw StoreError.invalidValue }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let inserted = try importVisibilityInTransaction(incoming)
        try database.execute("COMMIT"); committed = true
        return inserted
    }
    private func importVisibilityInTransaction(_ incoming: [LibraryVisibilityChange]) throws -> Int {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        var inserted = 0
        for change in incoming {
            try change.validate()
            let query = try database.query("SELECT identity,content,payload FROM library_visibility_events WHERE identity=?",
                                           [.text(change.id.uuidString)])
            if try query.next() {
                guard try decodeVisibility(query) == change else { throw LibraryVisibilityError.equivocation(change.id) }
            } else {
                let bytes = try encoder.encode(change)
                guard bytes.count <= 4096 else { throw StoreError.invalidValue }
                try database.execute("INSERT INTO library_visibility_events(identity,content,payload) VALUES(?,?,?)",
                                     [.text(change.id.uuidString), .text(change.content.hex), .blob(bytes)])
                inserted += 1
            }
        }
        let query = try database.query("SELECT identity,content,payload FROM library_visibility_events ORDER BY identity")
        var changes: [LibraryVisibilityChange] = []; changes.reserveCapacity(128)
        while try query.next() { changes.append(try decodeVisibility(query)) }
        for id in Set(changes.map(\.content)) {
            let state = try LibraryVisibilityHistory.merge(changes, content: id, initiallyRemoved: isLibraryContentDeleted(id))
            try applyVisibilityState(state, content: id)
        }
        return inserted
    }
    private func applyVisibilityState(_ state: LibraryVisibilitySnapshot, content id: ContentID) throws {
        if state.removed { try abortReaderImportsInTransaction(content: id) }
        guard try content(id) != nil else { return }
            if state.removed {
                try database.execute("INSERT INTO library_deletions(content) VALUES(?) ON CONFLICT(content) DO NOTHING", [.text(id.hex)])
                try database.execute("UPDATE reader_selections SET selected=0 WHERE content=? AND selected=1", [.text(id.hex)])
                try database.execute("""
                    INSERT INTO transfer_aborts(job) SELECT id FROM jobs WHERE content=?
                    AND phase NOT IN ('committing','completed','aborted') ON CONFLICT(job) DO NOTHING
                    """, [.text(id.hex)])
            } else { try database.execute("DELETE FROM library_deletions WHERE content=?", [.text(id.hex)]) }
    }
    private func applyRetainedVisibility(_ id: ContentID) throws {
        let query = try database.query("SELECT identity,content,payload FROM library_visibility_events WHERE content=?", [.text(id.hex)])
        var changes: [LibraryVisibilityChange] = []; changes.reserveCapacity(16)
        while try query.next() { changes.append(try decodeVisibility(query)) }
        guard !changes.isEmpty else { return }
        let state = try LibraryVisibilityHistory.merge(changes, content: id, initiallyRemoved: isLibraryContentDeleted(id))
        try applyVisibilityState(state, content: id)
    }
    @discardableResult
    public func journalLegacyLibraryRemovals(origin: Data, limit: Int = 128) throws -> Int {
        guard origin.count == 16, origin.contains(where: { $0 != 0 }), (1...250).contains(limit) else {
            throw StoreError.invalidValue
        }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let query = try database.query("""
            SELECT content FROM library_deletions WHERE NOT EXISTS
            (SELECT 1 FROM library_visibility_events WHERE library_visibility_events.content=library_deletions.content)
            ORDER BY content LIMIT ?
            """, [.integer(Int64(limit))])
        var changes: [LibraryVisibilityChange] = []; changes.reserveCapacity(limit)
        while try query.next() {
            changes.append(try LibraryVisibilityChange(origin: origin, content: ContentID(query.text(0)), removed: true, ancestors: []))
        }
        let inserted = try importVisibilityInTransaction(changes)
        try database.execute("COMMIT"); committed = true
        return inserted
    }
    @discardableResult
    public func setLibraryVisibility(_ id: ContentID, removed: Bool, origin: Data) throws -> Bool {
        guard origin.count == 16, origin.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try content(id) != nil else { throw StoreError.missingContent }
        let query = try database.query("SELECT identity,content,payload FROM library_visibility_events WHERE content=?", [.text(id.hex)])
        var changes: [LibraryVisibilityChange] = []; changes.reserveCapacity(16)
        while try query.next() { changes.append(try decodeVisibility(query)) }
        let baseline = try isLibraryContentDeleted(id)
        let state = try LibraryVisibilityHistory.merge(changes, content: id, initiallyRemoved: baseline)
        let legacy = baseline && state.heads.isEmpty
        if state.removed == removed && !legacy {
            try database.execute("COMMIT"); committed = true
            return false
        }
        var incoming: [LibraryVisibilityChange] = []; incoming.reserveCapacity(2)
        var heads = state.heads
        if legacy {
            let prior = try LibraryVisibilityChange(origin: origin, content: id, removed: true, ancestors: [])
            incoming.append(prior); heads = [prior.id]
        }
        if !legacy || !removed {
            incoming.append(try LibraryVisibilityChange(origin: origin, content: id, removed: removed, ancestors: heads))
        }
        _ = try importVisibilityInTransaction(incoming)
        try database.execute("COMMIT"); committed = true
        return true
    }
    @discardableResult
    public func deleteLibraryContent(_ id: ContentID) throws -> Bool {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try content(id) != nil else { throw StoreError.missingContent }
        if try isLibraryContentDeleted(id) {
            try database.execute("COMMIT"); committed = true
            return false
        }
        try database.execute("INSERT INTO library_deletions(content) VALUES(?)", [.text(id.hex)])
        try abortReaderImportsInTransaction(content: id)
        try database.execute("UPDATE reader_selections SET selected=0 WHERE content=? AND selected=1", [.text(id.hex)])
        try database.execute("""
            INSERT INTO transfer_aborts(job) SELECT id FROM jobs WHERE content=?
                AND phase NOT IN ('committing','completed','aborted') ON CONFLICT(job) DO NOTHING
            """, [.text(id.hex)])
        try database.execute("COMMIT"); committed = true
        return true
    }
    public func restoreLibraryContent(_ id: ContentID) throws {
        guard try content(id) != nil else { throw StoreError.missingContent }
        try database.execute("DELETE FROM library_deletions WHERE content=?", [.text(id.hex)])
    }
    @discardableResult
    public func setReaderSelection(reader: Data, content: ContentID, selected: Bool) throws -> Bool {
        guard reader.count == 16, reader.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard let object = try self.content(content) else { throw StoreError.missingContent }
        guard object.kind != .firmware else { throw StoreError.invalidValue }
        if selected { guard try !isLibraryContentDeleted(content) else { throw StoreError.invalidTransition } }
        try database.execute("""
            INSERT INTO reader_selections(reader,content,selected) VALUES(?,?,?)
            ON CONFLICT(reader,content) DO UPDATE SET selected=excluded.selected
            WHERE reader_selections.selected!=excluded.selected
            """, [.blob(reader), .text(content.hex), .integer(selected ? 1 : 0)])
        let changed = database.changedRows != 0
        if !selected {
            try database.execute("""
                UPDATE reader_import_jobs SET phase='aborted'
                WHERE reader=? AND content=? AND phase NOT IN ('completed','aborted')
                """, [.blob(reader), .text(content.hex)])
        }
        try database.execute("COMMIT"); committed = true
        return changed
    }
    public func readerSelections(reader: Data) throws -> [ReaderContentSelection] {
        guard reader.count == 16, reader.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
        let query = try database.query("SELECT content,selected FROM reader_selections WHERE reader=? ORDER BY content", [.blob(reader)])
        var selections: [ReaderContentSelection] = []; selections.reserveCapacity(32)
        while try query.next() {
            let value = query.integer(1)
            guard value == 0 || value == 1 else { throw StoreError.invalidValue }
            selections.append(ReaderContentSelection(content: try ContentID(query.text(0)), selected: value == 1))
        }
        return selections
    }
    public func put(_ content: LibraryContent) throws {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        try putInTransaction(content)
        try database.execute("COMMIT"); committed = true
    }
    private func putInTransaction(_ content: LibraryContent) throws {
        guard content.length <= UInt64(Int64.max) else { throw StoreError.invalidValue }
        if try pendingCloudCourseIdentity(content.id) != nil, content.kind != .course { throw StoreError.invalidValue }
        if let existing = try self.content(content.id), existing.length != content.length || existing.kind != content.kind {
            throw StoreError.invalidValue
        }
        try database.execute("""
            INSERT INTO content(hash,kind,length,title,filename,authors,identifiers,languages) VALUES(?,?,?,?,?,?,?,?)
            ON CONFLICT(hash) DO UPDATE SET title=excluded.title,filename=excluded.filename,
                authors=excluded.authors,identifiers=excluded.identifiers,languages=excluded.languages
            WHERE content.kind=excluded.kind AND content.length=excluded.length
            """, [.text(content.id.hex), .integer(content.kind.rawValue), .integer(Int64(content.length)),
                   .text(content.title), .text(content.originalFilename),
                   .text(try encodeStrings(content.authors)), .text(try encodeStrings(content.identifiers)),
                   .text(try encodeStrings(content.languages))])
        guard let stored = try self.content(content.id), stored.length == content.length, stored.kind == content.kind else {
            throw StoreError.invalidValue
        }
        try applyRetainedVisibility(content.id)
    }
    public func content(_ id: ContentID) throws -> LibraryContent? {
        let query = try database.query("SELECT kind,length,title,filename,authors,identifiers,languages FROM content WHERE hash=?", [.text(id.hex)])
        guard try query.next() else { return nil }
        guard let kind = ContentKind(rawValue: query.integer(0)), query.integer(1) >= 0 else { throw StoreError.invalidValue }
        return LibraryContent(id: id, kind: kind, length: UInt64(query.integer(1)), title: query.text(2), originalFilename: query.text(3),
                              authors: try decodeStrings(query.text(4)), identifiers: try decodeStrings(query.text(5)),
                              languages: try decodeStrings(query.text(6)))
    }
    public func saveReader(_ device: DeviceDescriptor, at date: Date = Date()) throws {
        let milliseconds = date.timeIntervalSince1970 * 1000
        guard milliseconds.isFinite, milliseconds >= 0, milliseconds < Double(Int64.max),
              device.identity.count == 16, device.identity.contains(where: { $0 != 0 }),
              device.storageGeneration.count == 16, device.storageGeneration.contains(where: { $0 != 0 }) else {
            throw StoreError.invalidValue
        }
        guard try DeviceDescriptor(decoding: device.encoded) == device else { throw StoreError.invalidValue }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        do {
            let previous = try database.query("SELECT descriptor FROM readers WHERE identity=?", [.blob(device.identity)])
            if try previous.next(), try DeviceDescriptor(decoding: previous.blob(0)).storageGeneration != device.storageGeneration {
                try database.execute("UPDATE readers SET last_sync=NULL WHERE identity=?", [.blob(device.identity)])
            }
        }
        try database.execute("""
            INSERT INTO readers(identity,descriptor,last_connected) VALUES(?,?,?)
            ON CONFLICT(identity) DO UPDATE SET descriptor=excluded.descriptor,last_connected=excluded.last_connected
            WHERE readers.descriptor!=excluded.descriptor OR readers.last_connected!=excluded.last_connected
            """, [.blob(device.identity), .blob(device.encoded), .integer(Int64(milliseconds))])
        try database.execute("COMMIT"); committed = true
    }
    @discardableResult
    public func recordSuccessfulReaderSync(_ checkpoint: VerifiedReaderJournalCheckpoint, inventory: ReaderInventory,
                                           at date: Date = Date()) throws -> Bool {
        let milliseconds = date.timeIntervalSince1970 * 1000
        guard milliseconds.isFinite, milliseconds >= 0, milliseconds < Double(Int64.max),
              inventory.complete, inventory.reader == checkpoint.reader, inventory.generation == checkpoint.generation,
              checkpoint.mutations.count == Int(checkpoint.snapshot.count),
              Set(checkpoint.mutations.map(\.event.identity)).count == checkpoint.mutations.count,
              try TintaJournalFrontier.digest(checkpoint.mutations) == checkpoint.snapshot.frontier else {
            throw StoreError.invalidValue
        }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let saved = try database.query("SELECT descriptor FROM readers WHERE identity=?", [.blob(checkpoint.reader)])
        let imports = try database.query("""
            SELECT 1 FROM reader_import_jobs WHERE reader=? AND generation=?
            AND phase NOT IN ('completed','aborted') LIMIT 1
            """, [.blob(checkpoint.reader), .blob(checkpoint.generation)])
        guard try saved.next(), try DeviceDescriptor(decoding: saved.blob(0)).storageGeneration == checkpoint.generation,
              let baseline = try readerJournalBaseline(reader: checkpoint.reader, generation: checkpoint.generation),
              baseline.frontier == checkpoint.snapshot.frontier, baseline.count == checkpoint.snapshot.count,
              try pendingJournalMerge(reader: checkpoint.reader, generation: checkpoint.generation) == nil,
              try pendingTintaMigration(reader: checkpoint.reader, generation: checkpoint.generation) == nil,
              try pendingTintaInstallation(reader: checkpoint.reader, generation: checkpoint.generation) == nil,
              try reconcileContent(reader: checkpoint.reader, generation: checkpoint.generation, inventory: inventory).isEmpty,
              try !pendingJobs().contains(where: { $0.reader == checkpoint.reader && $0.storageGeneration == checkpoint.generation && $0.phase != .aborted }),
              try !pendingRemovalJobs().contains(where: { $0.reader == checkpoint.reader && $0.request.generation == checkpoint.generation }),
              try !imports.next(),
              try !preferences().contains(where: { $0.requiresResolution }) else { return false }
        let prefix = Set(checkpoint.mutations.map(\.event.identity))
        for mutation in checkpoint.mutations {
            guard try storedEvent(mutation.event.identity) == mutation else { throw StoreError.invalidValue }
        }
        let books = Set(inventory.contents.filter { $0.kind == .epub }.map { $0.content.digest })
        let courses = Set(inventory.contents.filter { $0.kind == .course }.map(\.logicalIdentity))
        for mutation in try journalMutations() where !prefix.contains(mutation.event.identity) {
            if mutation.event.kind == .preference ||
                (mutation.event.kind.rawValue < SyncEventKind.preference.rawValue && books.contains(mutation.event.resource)) {
                return false
            }
            if mutation.event.kind.rawValue >= SyncEventKind.review.rawValue,
               courses.contains(try TintaBody(mutation: mutation).subject.course) { return false }
        }
        for book in books {
            if try readingPositions(content: book).requiresResolution || bookmarks(content: book).contains(where: { $0.requiresResolution }) {
                return false
            }
        }
        try database.execute("UPDATE readers SET last_sync=? WHERE identity=? AND (last_sync IS NULL OR last_sync<?)",
            [.integer(Int64(milliseconds)), .blob(checkpoint.reader), .integer(Int64(milliseconds))])
        try database.execute("COMMIT"); committed = true
        return true
    }
    public func savedReaders() throws -> [SavedReader] {
        let query = try database.query("""
            SELECT identity,descriptor,last_connected,COALESCE(last_sync,-1)
            FROM readers ORDER BY last_connected DESC,identity
            """)
        var result: [SavedReader] = []; result.reserveCapacity(4)
        while try query.next() {
            let device = try DeviceDescriptor(decoding: query.blob(1))
            guard device.identity == query.blob(0), device.identity.contains(where: { $0 != 0 }),
                  device.storageGeneration.contains(where: { $0 != 0 }), query.integer(2) >= 0,
                  query.integer(3) >= -1 else { throw StoreError.invalidValue }
            result.append(SavedReader(device: device, lastConnected: Date(timeIntervalSince1970: Double(query.integer(2)) / 1000),
                lastSuccessfulSync: query.integer(3) == -1 ? nil : Date(timeIntervalSince1970: Double(query.integer(3)) / 1000)))
        }
        return result
    }
    public func putFirmware(_ content: LibraryContent, releaseAsset: Data) throws {
        guard releaseAsset.count <= 16384 else { throw StoreError.invalidValue }
        let metadata = try JSONDecoder().decode(FirmwareReleaseCompatibility.self, from: releaseAsset)
        guard content.kind == .firmware, content.length == metadata.length, content.id.digest == metadata.sha256 else {
            throw StoreError.invalidValue
        }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        if let previous = try firmwareCompatibility(content.id), previous != metadata { throw StoreError.invalidValue }
        try putInTransaction(content)
        try database.execute("INSERT INTO firmware_assets(content,metadata) VALUES(?,?) ON CONFLICT(content) DO NOTHING",
                             [.text(content.id.hex), .blob(releaseAsset)])
        try database.execute("COMMIT"); committed = true
    }
    public func firmwareCompatibility(_ id: ContentID) throws -> FirmwareReleaseCompatibility? {
        let query = try database.query("SELECT metadata FROM firmware_assets WHERE content=?", [.text(id.hex)])
        guard try query.next() else { return nil }
        let metadata = try JSONDecoder().decode(FirmwareReleaseCompatibility.self, from: query.blob(0))
        guard let stored = try content(id), stored.kind == .firmware, stored.length == metadata.length,
              id.digest == metadata.sha256 else { throw StoreError.invalidValue }
        return metadata
    }

    public func putCoursePack(_ content: LibraryContent, metadata: CoursePackMetadata) throws {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        try putCoursePackInTransaction(content, metadata: metadata)
        try database.execute("COMMIT"); committed = true
    }
    private func putCoursePackInTransaction(_ content: LibraryContent, metadata: CoursePackMetadata) throws {
        let details = try CoursePackDetails(metadata)
        guard content.kind == .course, content.languages == [details.locale] else { throw StoreError.invalidValue }
        if let existing = try coursePackDetails(content.id), existing != details { throw StoreError.invalidValue }
        try putInTransaction(content)
        try database.execute("""
            INSERT INTO course_packs(content,major,minor,edition,locale) VALUES(?,?,?,?,?)
            ON CONFLICT(content) DO NOTHING
            """, [.text(content.id.hex), .integer(Int64(details.major)), .integer(Int64(details.minor)),
                   .integer(Int64(details.contentVersion)), .text(details.locale)])
        guard try coursePackDetails(content.id) == details else { throw StoreError.invalidValue }
        if let identity = try pendingCloudCourseIdentity(content.id) {
            try requireCourseAssociation(resource: content.id.digest, course: identity)
            try database.execute("INSERT INTO course_associations(content,identity) VALUES(?,?) ON CONFLICT(content) DO NOTHING",
                                 [.text(content.id.hex), .blob(identity)])
        }
    }
    public func coursePackDetails(_ id: ContentID) throws -> CoursePackDetails? {
        let query = try database.query("SELECT major,minor,edition,locale FROM course_packs WHERE content=?", [.text(id.hex)])
        guard try query.next() else { return nil }
        guard (0 ... Int64(UInt16.max)).contains(query.integer(0)),
              (0 ... Int64(UInt16.max)).contains(query.integer(1)),
              (0 ... Int64(UInt32.max)).contains(query.integer(2)),
              try content(id)?.kind == .course else { throw StoreError.invalidValue }
        return try CoursePackDetails(major: UInt16(query.integer(0)), minor: UInt16(query.integer(1)),
            contentVersion: UInt32(query.integer(2)), locale: query.text(3))
    }
    @discardableResult
    public func associateCourse(_ id: ContentID, confirmedIdentity: Data) throws -> Bool {
        guard confirmedIdentity.count == 16, confirmedIdentity.contains(where: { $0 != 0 }) else {
            throw StoreError.invalidValue
        }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try coursePackDetails(id) != nil, try !isLibraryContentDeleted(id) else { throw StoreError.missingContent }
        try requireCourseAssociation(resource: id.digest, course: confirmedIdentity)
        if let existing = try courseIdentity(id) {
            guard existing == confirmedIdentity else { throw StoreError.invalidValue }
            try database.execute("COMMIT"); committed = true
            return false
        }
        try database.execute("INSERT INTO course_associations(content,identity) VALUES(?,?)",
                             [.text(id.hex), .blob(confirmedIdentity)])
        try database.execute("COMMIT"); committed = true
        return true
    }
    public func courseIdentity(_ id: ContentID) throws -> Data? {
        let query = try database.query("SELECT identity FROM course_associations WHERE content=?", [.text(id.hex)])
        guard try query.next() else { return nil }
        let identity = query.blob(0)
        guard identity.count == 16, identity.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
        return identity
    }
    private func requireCourseAssociation(resource: Data, course: Data) throws {
        guard resource.count == 32, course.count == 16, course.contains(where: { $0 != 0 }) else {
            throw StoreError.invalidValue
        }
        let id = try ContentID(resource.map { String(format: "%02x", $0) }.joined())
        if let existing = try courseIdentity(id), existing != course { throw StoreError.invalidValue }
        if let pending = try pendingCloudCourseIdentity(id), pending != course { throw StoreError.invalidValue }
        if let details = try coursePackDetails(id) {
            let locales = try database.query("""
                SELECT p.locale FROM course_packs p JOIN (
                    SELECT content FROM course_associations WHERE identity=?
                    UNION SELECT content FROM cloud_course_bindings WHERE identity=?
                ) a ON a.content=p.content
                """, [.blob(course), .blob(course)])
            while try locales.next() {
                guard locales.text(0).lowercased() == details.locale.lowercased() else { throw StoreError.invalidValue }
            }
        }
        let conflict = try database.query("""
            SELECT 1 FROM legacy_migrations m JOIN legacy_backups b ON b.hash=m.backup
            WHERE m.resource=? AND b.course!=? LIMIT 1
            """, [.blob(resource), .blob(course)])
        guard try !conflict.next() else { throw StoreError.invalidValue }
        let events = try database.query("SELECT identity,resource,envelope,body FROM sync_events WHERE resource=?",
                                        [.blob(resource)])
        while try events.next() {
            let mutation = try decodeMutation(events)
            if mutation.event.kind.rawValue >= SyncEventKind.review.rawValue {
                guard try TintaBody(mutation: mutation).subject.course == course else { throw StoreError.invalidValue }
            }
        }
    }
    private func pendingCloudCourseIdentity(_ id: ContentID) throws -> Data? {
        let query = try database.query("SELECT identity FROM cloud_course_bindings WHERE content=?", [.text(id.hex)])
        guard try query.next() else { return nil }
        let identity = query.blob(0)
        _ = try CloudCourseAssociation(content: id, identity: identity)
        return identity
    }
    public func acceptCloudCourseAssociation(_ association: CloudCourseAssociation) throws {
        try association.validate()
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        if let content = try self.content(association.content), content.kind != .course { throw StoreError.invalidValue }
        try requireCourseAssociation(resource: association.content.digest, course: association.identity)
        try database.execute("INSERT INTO cloud_course_bindings(content,identity) VALUES(?,?) ON CONFLICT(content) DO NOTHING",
                             [.text(association.content.hex), .blob(association.identity)])
        if try coursePackDetails(association.content) != nil {
            try database.execute("INSERT INTO course_associations(content,identity) VALUES(?,?) ON CONFLICT(content) DO NOTHING",
                                 [.text(association.content.hex), .blob(association.identity)])
        }
        try database.execute("COMMIT"); committed = true
    }
    public func cloudCourseAssociations(after id: ContentID? = nil, limit: Int = 128) throws -> [CloudCourseAssociation] {
        guard (1...128).contains(limit) else { throw StoreError.invalidValue }
        let query = try database.query("""
            SELECT content,identity FROM course_associations WHERE content>?
            AND NOT EXISTS(SELECT 1 FROM library_deletions WHERE library_deletions.content=course_associations.content)
            ORDER BY content LIMIT ?
            """, [.text(id?.hex ?? ""), .integer(Int64(limit))])
        var result: [CloudCourseAssociation] = []; result.reserveCapacity(limit)
        while try query.next() {
            result.append(try CloudCourseAssociation(content: ContentID(query.text(0)), identity: query.blob(1)))
        }
        return result
    }
    public func pendingCloudCourseAssociations(_ page: [CloudCourseAssociation], account: String) throws -> [CloudCourseAssociation] {
        guard page.count <= 128 else { throw StoreError.invalidValue }
        let key = try cloudAccountKey(account)
        var result: [CloudCourseAssociation] = []; result.reserveCapacity(page.count)
        for association in page {
            try association.validate()
            let query = try database.query("SELECT identity FROM cloud_course_receipts WHERE account=? AND content=?",
                                           [.blob(key), .text(association.content.hex)])
            if try query.next() {
                guard query.blob(0) == association.identity else { throw StoreError.invalidValue }
            } else { result.append(association) }
        }
        return result
    }
    public func acknowledgeCloudCourseAssociation(_ association: CloudCourseAssociation, account: String) throws {
        try association.validate()
        let key = try cloudAccountKey(account)
        guard try courseIdentity(association.content) ?? pendingCloudCourseIdentity(association.content) == association.identity else {
            throw StoreError.invalidValue
        }
        let query = try database.query("SELECT identity FROM cloud_course_receipts WHERE account=? AND content=?",
                                       [.blob(key), .text(association.content.hex)])
        if try query.next() {
            guard query.blob(0) == association.identity else { throw StoreError.invalidValue }
        } else {
            try database.execute("INSERT INTO cloud_course_receipts(account,content,identity) VALUES(?,?,?)",
                                 [.blob(key), .text(association.content.hex), .blob(association.identity)])
        }
    }
    public func courseManifest(_ id: ContentID) throws -> ContentManifest {
        guard try !isLibraryContentDeleted(id) else { throw StoreError.missingContent }
        return try storedCourseManifest(id)
    }
    private func storedCourseManifest(_ id: ContentID) throws -> ContentManifest {
        guard let content = try content(id), content.kind == .course,
              let details = try coursePackDetails(id), let identity = try courseIdentity(id) else {
            throw StoreError.missingContent
        }
        try requireCourseAssociation(resource: id.digest, course: identity)
        return try ContentManifest(content: id, kind: .course, length: content.length,
                                   formatVersion: UInt32(details.major), logicalIdentity: identity)
    }
    public func importLegacyPreferences(backup: ContentID, origin: Data, expected: [PreferenceBody],
                                        vault: ContentVault) async throws -> [JournalMutation] {
        guard origin.count == 16, origin.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
        let snapshot = try await vault.legacyBackupSnapshot(backup)
        let preferences = try snapshot.profile.portablePreferences()
        guard expected == preferences, preferences.count == 9 else { throw StoreError.invalidValue }
        let canonical = try sharedLegacyCanonical(backup)
        if let canonical {
            let shared = try await vault.legacyBackupSnapshot(canonical)
            guard try shared.profile.portablePreferences() == preferences else { throw StoreError.invalidValue }
        }
        let receiptBackup = canonical ?? backup
        var payload = Data(); payload.reserveCapacity(72)
        for preference in preferences { payload.append(preference.encoded) }
        guard payload.count == 72 else { throw StoreError.invalidValue }
        try Task.checkCancellation()
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let registered = try database.query("SELECT reader,generation,course FROM legacy_backups WHERE hash=?", [.text(backup.hex)])
        guard try registered.next(), registered.blob(0) == snapshot.manifest.reader,
              registered.blob(1) == snapshot.manifest.generation, registered.blob(2) == snapshot.manifest.course else {
            throw StoreError.invalidValue
        }
        guard try sharedLegacyCanonical(backup) == canonical else { throw StoreError.conflictingJob }
        var result: [JournalMutation] = []; result.reserveCapacity(9)
        do {
            let receipt = try database.query("SELECT preferences,identities FROM legacy_preference_imports WHERE backup=?", [.text(receiptBackup.hex)])
            if try receipt.next() {
                guard receipt.blob(0) == payload, receipt.blob(1).count == 288 else { throw StoreError.invalidValue }
                var reader = ByteReader(receipt.blob(1))
                for preference in preferences {
                    let identity = try EventIdentity(origin: reader.take(16), epoch: reader.number(8), sequence: reader.number(8))
                    guard let mutation = try storedEvent(identity), mutation.event.kind == .preference,
                          mutation.event.resource == PreferenceBody.scope, mutation.body == preference.encoded else {
                        throw StoreError.invalidValue
                    }
                    result.append(mutation)
                }
                guard Set(result.map(\.event.identity)).count == 9 else { throw StoreError.invalidValue }
                try database.execute("COMMIT"); committed = true
                return result
            }
        }
        var identities = Data(); identities.reserveCapacity(288)
        for preference in preferences {
            let mutation = try createEventInTransaction(origin: origin, resource: PreferenceBody.scope,
                kind: .preference, body: preference.encoded, ancestors: [])
            result.append(mutation); identities.append(mutation.event.identity.storageKey)
        }
        try database.execute("INSERT INTO legacy_preference_imports(backup,preferences,identities) VALUES(?,?,?)",
            [.text(receiptBackup.hex), .blob(payload), .blob(identities)])
        try database.execute("COMMIT"); committed = true
        return result
    }
    public func appendReadingPosition(origin: Data, content: Data, anchor: ReadingAnchor,
                                      ancestors: [EventIdentity] = []) throws -> JournalMutation {
        try createEvent(origin: origin, resource: content, kind: .readingPosition, body: anchor.encoded, ancestors: ancestors)
    }
    public func appendBookmark(origin: Data, content: Data, bookmark: BookmarkBody,
                               ancestors: [EventIdentity] = []) throws -> JournalMutation {
        try createEvent(origin: origin, resource: content, kind: bookmark.kind, body: bookmark.encoded, ancestors: ancestors)
    }
    public func appendPreference(origin: Data, preference: PreferenceBody,
                                 ancestors: [EventIdentity] = []) throws -> JournalMutation {
        try createEvent(origin: origin, resource: PreferenceBody.scope, kind: .preference, body: preference.encoded, ancestors: ancestors)
    }
    public func appendPreferences(origin: Data, preferences: [PreferenceBody]) throws -> [JournalMutation] {
        guard Set(preferences.map(\.key)).count == preferences.count else { throw StoreError.invalidValue }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        var result: [JournalMutation] = []; result.reserveCapacity(preferences.count)
        for preference in preferences.sorted(by: { $0.key.rawValue < $1.key.rawValue }) {
            result.append(try createEventInTransaction(origin: origin, resource: PreferenceBody.scope,
                kind: .preference, body: preference.encoded, ancestors: []))
        }
        try database.execute("COMMIT"); committed = true
        return result
    }
    public func resolveReadingPosition(origin: Data, content: Data, anchor: ReadingAnchor,
                                       expectedHeads: [EventIdentity]) throws -> [JournalMutation] {
        try resolve(origin: origin, resource: content, kind: .readingPosition, body: anchor.encoded, expectedHeads: expectedHeads)
    }
    public func resolveBookmark(origin: Data, content: Data, bookmark: BookmarkBody,
                                expectedHeads: [EventIdentity]) throws -> [JournalMutation] {
        try resolve(origin: origin, resource: content, kind: bookmark.kind, body: bookmark.encoded, expectedHeads: expectedHeads)
    }
    public func resolvePreference(origin: Data, preference: PreferenceBody,
                                  expectedHeads: [EventIdentity]) throws -> [JournalMutation] {
        try resolve(origin: origin, resource: PreferenceBody.scope, kind: .preference, body: preference.encoded, expectedHeads: expectedHeads)
    }
    private func resolve(origin: Data, resource: Data, kind: SyncEventKind, body: Data,
                         expectedHeads: [EventIdentity]) throws -> [JournalMutation] {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let current: [EventIdentity]
        switch kind {
        case .readingPosition: current = try readingPositions(content: resource).resolutionAncestors
        case .bookmarkPut, .bookmarkDelete:
            let bookmark = try BookmarkBody(decoding: body)
            current = try bookmarks(content: resource).first { $0.identity == bookmark.identity }?.resolutionAncestors ?? []
        case .preference:
            let preference = try PreferenceBody(decoding: body)
            current = try preferences().first { $0.key == preference.key }?.resolutionAncestors ?? []
        default: throw ProtocolError.value
        }
        let expected = Set(expectedHeads)
        guard !expected.isEmpty, expected.count == expectedHeads.count, expected == Set(current) else { throw HistoryError.staleFrontier }
        let heads = expected.sorted()
        var created: [JournalMutation] = []; created.reserveCapacity((heads.count + 3) / 4)
        for offset in stride(from: 0, to: heads.count, by: 4) {
            let ancestors = Array(heads[offset ..< min(offset + 4, heads.count)])
            created.append(try createEventInTransaction(origin: origin, resource: resource, kind: kind, body: body, ancestors: ancestors))
        }
        try database.execute("COMMIT"); committed = true
        return created
    }
    private func createEvent(origin: Data, resource: Data, kind: SyncEventKind, body: Data,
                             ancestors: [EventIdentity]) throws -> JournalMutation {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let mutation = try createEventInTransaction(origin: origin, resource: resource, kind: kind, body: body, ancestors: ancestors)
        try database.execute("COMMIT"); committed = true
        return mutation
    }
    private func createEventInTransaction(origin: Data, resource: Data, kind: SyncEventKind, body: Data,
                                          ancestors: [EventIdentity]) throws -> JournalMutation {
        guard origin.count == 16, origin.contains(where: { $0 != 0 }), resource.count == 32,
              resource.contains(where: { $0 != 0 }), ancestors.count <= 4 else { throw ProtocolError.value }
        let epoch: UInt64, generation: Data, previous: UInt64
        do {
            let query = try database.query("SELECT epoch,generation,sequence FROM local_origins WHERE origin=?", [.blob(origin)])
            if try query.next() {
                var epochReader = ByteReader(query.blob(0)), sequenceReader = ByteReader(query.blob(2))
                guard epochReader.bytes.count == 8, sequenceReader.bytes.count == 8 else { throw StoreError.invalidValue }
                epoch = try epochReader.number(8); previous = try sequenceReader.number(8); generation = query.blob(1)
                guard epoch > 0, previous > 0, generation.count == 16, generation.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
            } else {
                let epochBytes = try random(8)
                guard epochBytes.count == 8 else { throw CredentialError.invalidRandom }
                var reader = ByteReader(epochBytes); epoch = try reader.number(8)
                let collision = try database.query("""
                    SELECT 1 FROM legacy_migrations WHERE origin=? AND epoch=?
                    UNION ALL SELECT 1 FROM sync_events WHERE substr(identity,1,24)=? LIMIT 1
                    """, [.blob(origin), .blob(epochBytes), .blob(origin + epochBytes)])
                guard try !collision.next() else { throw StoreError.invalidValue }
                generation = try random(16); previous = 0
                guard epoch > 0, generation.count == 16, generation.contains(where: { $0 != 0 }) else { throw CredentialError.invalidRandom }
            }
        }
        guard previous < UInt64.max else { throw StoreError.invalidValue }
        let identity = try EventIdentity(origin: origin, epoch: epoch, sequence: previous + 1)
        let event = try SyncEvent(identity: identity, storageGeneration: generation, kind: kind, resource: resource,
                                  bodyHash: Data(SHA256.hash(data: body)), ancestors: ancestors)
        let mutation = try JournalMutation(event: event, body: body)
        var required = ancestors
        required.reserveCapacity(5)
        if previous > 0 { required.append(try EventIdentity(origin: origin, epoch: epoch, sequence: previous)) }
        _ = try SyncHistory.merged(causalDependencies(required) + [event])
        try database.execute("INSERT INTO sync_events(identity,resource,envelope,body) VALUES(?,?,?,?)",
                             [.blob(identity.storageKey), .blob(resource), .blob(event.bytes), .blob(body)])
        var epochBytes = Data(), sequenceBytes = Data()
        epochBytes.appendLittleEndian(epoch, count: 8); sequenceBytes.appendLittleEndian(identity.sequence, count: 8)
        try database.execute("""
            INSERT INTO local_origins(origin,epoch,generation,sequence) VALUES(?,?,?,?)
            ON CONFLICT(origin) DO UPDATE SET sequence=excluded.sequence
            """, [.blob(origin), .blob(epochBytes), .blob(generation), .blob(sequenceBytes)])
        return mutation
    }
    private func causalDependencies(_ roots: [EventIdentity]) throws -> [SyncEvent] {
        var pending = roots; pending.reserveCapacity(256)
        var visited = Set<EventIdentity>(); visited.reserveCapacity(256)
        var events: [SyncEvent] = []; events.reserveCapacity(256)
        while let identity = pending.popLast() {
            guard visited.insert(identity).inserted else { continue }
            guard let mutation = try storedEvent(identity) else { throw HistoryError.missingAncestor(identity) }
            let event = mutation.event; events.append(event)
            pending.append(contentsOf: event.ancestors)
            if identity.sequence > 1 {
                pending.append(try EventIdentity(origin: identity.origin, epoch: identity.epoch, sequence: identity.sequence - 1))
            }
        }
        return events
    }
    @discardableResult
    public func importTintaEvents(_ mutations: [JournalMutation]) throws -> Int {
        for mutation in mutations { _ = try TintaBody(mutation: mutation) }
        return try importEvents(mutations)
    }
    public func preferences() throws -> [PreferenceState] {
        try PreferenceHistory.reconcile(journalMutations())
    }
    public func bookmarks(content: Data) throws -> [BookmarkState] {
        try BookmarkHistory.reconcile(journalMutations(), content: content)
    }
    public func readingPositions(content: Data) throws -> ReadingPositions {
        try ReadingHistory.reconcile(journalMutations(), content: content)
    }
    public func persistTintaDerivedInstallation(content: ContentID, vault: ContentVault, studyDay: UInt16,
                                               storageGeneration: Data, snapshotIdentity: Data,
                                               revision: UInt64) async throws -> ContentID {
        let installation = try await tintaDerivedInstallation(content: content, vault: vault, studyDay: studyDay,
            storageGeneration: storageGeneration, snapshotIdentity: snapshotIdentity, revision: revision)
        let stored = try await vault.storeTintaDerivedInstallation(installation)
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let pack = try courseManifest(content)
        guard pack.logicalIdentity == installation.files.course,
              try TintaDerivedReceipt(decoding: installation.manifest).packHash == content.digest,
              try TintaJournalFrontier.digest(journalMutations()) == installation.frontierHash else {
            throw HistoryError.staleFrontier
        }
        try database.execute("""
            INSERT INTO tinta_artifacts(manifest,payload) VALUES(?,?) ON CONFLICT(manifest) DO NOTHING
            """, [.text(stored.id.hex), .blob(installation.manifest)])
        let query = try database.query("SELECT payload FROM tinta_artifacts WHERE manifest=?", [.text(stored.id.hex)])
        guard try query.next(), query.blob(0) == installation.manifest else { throw StoreError.invalidValue }
        try database.execute("COMMIT")
        committed = true
        return stored.id
    }
    public func restoreRetainedTintaDerivedInstallation(manifest: ContentID, vault: ContentVault,
                                                       storageGeneration: Data, maximumBytes: UInt64
    ) async throws -> TintaDerivedInstallation {
        let bytes = try retainedTintaManifest(manifest)
        let receipt = try TintaDerivedReceipt(decoding: bytes)
        guard receipt.storageGeneration == storageGeneration else { throw StoreError.invalidValue }
        let packID = try ContentID(receipt.packHash.map { String(format: "%02x", $0) }.joined())
        let pack = try courseManifest(packID)
        guard pack.logicalIdentity == receipt.course,
              try TintaJournalFrontier.digest(journalMutations()) == receipt.frontierHash else {
            throw HistoryError.staleFrontier
        }
        let installation = try await vault.restoreTintaDerivedInstallation(manifest: manifest, maximumBytes: maximumBytes)
        let packObject = try await vault.verifiedObject(packID)
        guard installation.manifest == bytes, packObject.length == pack.length,
              try retainedTintaManifest(manifest) == bytes,
              try courseManifest(packID) == pack,
              try TintaJournalFrontier.digest(journalMutations()) == receipt.frontierHash else {
            throw HistoryError.staleFrontier
        }
        return installation
    }
    // Restores the immutable generation for recovery of an already dispatched commit.
    public func restoreCommittingTintaInstallation(_ request: PendingTintaInstallation, vault: ContentVault,
                                                   maximumBytes: UInt64) async throws -> TintaDerivedInstallation {
        guard request.phase == .committing,
              try pendingTintaInstallation(reader: request.reader, generation: request.storageGeneration) == request else {
            throw StoreError.conflictingJob
        }
        let bytes = try retainedTintaManifest(request.manifest)
        let receipt = try TintaDerivedReceipt(decoding: bytes)
        guard receipt.storageGeneration == request.storageGeneration else { throw VaultError.integrity }
        let packID = try ContentID(receipt.packHash.map { String(format: "%02x", $0) }.joined())
        let installation = try await vault.restoreTintaDerivedInstallation(manifest: request.manifest, maximumBytes: maximumBytes)
        _ = try await vault.verifiedObject(packID)
        guard installation.manifest == bytes,
              try retainedTintaManifest(request.manifest) == bytes,
              try pendingTintaInstallation(reader: request.reader, generation: request.storageGeneration) == request else {
            throw StoreError.conflictingJob
        }
        return installation
    }
    private func retainedTintaManifest(_ manifest: ContentID) throws -> Data {
        let query = try database.query("SELECT payload FROM tinta_artifacts WHERE manifest=?", [.text(manifest.hex)])
        guard try query.next() else { throw StoreError.missingContent }
        let bytes = query.blob(0)
        guard Data(SHA256.hash(data: bytes)) == manifest.digest else { throw VaultError.integrity }
        return bytes
    }
    public func enqueueTintaInstallation(manifest: ContentID, inventory: ReaderInventory, owner: Data,
                                         transaction: UUID = UUID()) throws -> PendingTintaInstallation {
        guard inventory.complete else { throw ContentReconciliationError.incompleteInventory }
        guard owner.count == 16, owner.contains(where: { $0 != 0 }),
              transaction.uuidString != "00000000-0000-0000-0000-000000000000" else { throw StoreError.invalidValue }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let receipt = try TintaDerivedReceipt(decoding: retainedTintaManifest(manifest))
        guard receipt.storageGeneration == inventory.generation else { throw ContentReconciliationError.wrongReader }
        let packID = try ContentID(receipt.packHash.map { String(format: "%02x", $0) }.joined())
        let pack = try courseManifest(packID)
        guard pack.logicalIdentity == receipt.course, inventory.contents.filter({ $0.kind == .course }) == [pack],
              try TintaJournalFrontier.digest(journalMutations()) == receipt.frontierHash else {
            throw HistoryError.staleFrontier
        }
        guard try pendingTintaMigration(reader: inventory.reader, generation: inventory.generation) == nil,
              try pendingJournalMerge(reader: inventory.reader, generation: inventory.generation) == nil else {
            throw StoreError.conflictingJob
        }
        if let pending = try pendingTintaInstallation(reader: inventory.reader, generation: inventory.generation) {
            guard pending.manifest == manifest, pending.owner == owner else { throw StoreError.conflictingJob }
            try database.execute("COMMIT"); committed = true
            return pending
        }
        let existing = try database.query("SELECT 1 FROM tinta_installation_queue WHERE transaction_id=?", [.text(transaction.uuidString)])
        guard try !existing.next() else { throw StoreError.conflictingJob }
        try database.execute("INSERT INTO tinta_installation_queue(transaction_id,manifest,reader,generation,owner) VALUES(?,?,?,?,?)",
            [.text(transaction.uuidString), .text(manifest.hex), .blob(inventory.reader), .blob(inventory.generation), .blob(owner)])
        try database.execute("COMMIT"); committed = true
        return PendingTintaInstallation(transaction: transaction, manifest: manifest, reader: inventory.reader,
            storageGeneration: inventory.generation, owner: owner)
    }
    public func pendingTintaInstallation(reader: Data, generation: Data) throws -> PendingTintaInstallation? {
        let query = try database.query("SELECT transaction_id,manifest,owner,phase FROM tinta_installation_queue WHERE reader=? AND generation=?",
            [.blob(reader), .blob(generation)])
        guard try query.next() else { return nil }
        guard let transaction = UUID(uuidString: query.text(0)), let phase = TintaInstallationPhase(rawValue: query.text(3)) else { throw StoreError.invalidValue }
        return try PendingTintaInstallation(transaction: transaction, manifest: ContentID(query.text(1)), reader: reader,
            storageGeneration: generation, owner: query.blob(2), phase: phase)
    }
    public func claimTintaInstallation(_ request: PendingTintaInstallation, inventory: ReaderInventory) throws -> PendingTintaInstallation {
        try transitionTintaInstallation(request, inventory: inventory, phase: .staging)
    }
    public func prepareTintaInstallationCommit(_ request: PendingTintaInstallation, inventory: ReaderInventory) throws -> PendingTintaInstallation {
        try transitionTintaInstallation(request, inventory: inventory, phase: .committing)
    }
    private func transitionTintaInstallation(_ request: PendingTintaInstallation, inventory: ReaderInventory,
                                             phase: TintaInstallationPhase) throws -> PendingTintaInstallation {
        guard inventory.complete, inventory.reader == request.reader, inventory.generation == request.storageGeneration else {
            throw ContentReconciliationError.wrongReader
        }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard let pending = try pendingTintaInstallation(reader: request.reader, generation: request.storageGeneration),
              pending.transaction == request.transaction, pending.manifest == request.manifest, pending.owner == request.owner else {
            throw StoreError.conflictingJob
        }
        if pending.phase == .committing {
            guard phase == .committing else { throw StoreError.conflictingJob }
            try database.execute("COMMIT"); committed = true
            return pending
        }
        guard phase != .committing || pending.phase == .staging else { throw StoreError.conflictingJob }
        let receipt = try TintaDerivedReceipt(decoding: retainedTintaManifest(pending.manifest))
        let packID = try ContentID(receipt.packHash.map { String(format: "%02x", $0) }.joined())
        let pack = try courseManifest(packID)
        guard receipt.storageGeneration == inventory.generation, receipt.course == pack.logicalIdentity,
              inventory.contents.filter({ $0.kind == .course }) == [pack],
              try TintaJournalFrontier.digest(journalMutations()) == receipt.frontierHash else { throw HistoryError.staleFrontier }
        try database.execute("UPDATE tinta_installation_queue SET phase=? WHERE transaction_id=?", [.text(phase.rawValue), .text(pending.transaction.uuidString)])
        try database.execute("COMMIT"); committed = true
        return PendingTintaInstallation(transaction: pending.transaction, manifest: pending.manifest, reader: pending.reader,
            storageGeneration: pending.storageGeneration, owner: pending.owner, phase: phase)
    }
    // Caller supplies a committed receipt received through the authenticated reader session.
    @discardableResult
    public func confirmTintaInstallation(_ request: PendingTintaInstallation, committedManifest: Data,
                                          reader: Data, generation: Data, owner: Data) throws -> Bool {
        guard request.phase == .committing, reader == request.reader,
              generation == request.storageGeneration, owner == request.owner else {
            throw ContentReconciliationError.wrongReader
        }
        let receipt = try TintaDerivedReceipt(decoding: committedManifest)
        guard receipt.storageGeneration == generation,
              Data(SHA256.hash(data: committedManifest)) == request.manifest.digest else { throw VaultError.integrity }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard let pending = try pendingTintaInstallation(reader: reader, generation: generation) else {
            try database.execute("COMMIT"); committed = true
            return false
        }
        guard pending == request else { throw StoreError.conflictingJob }
        guard try retainedTintaManifest(request.manifest) == committedManifest else { throw VaultError.integrity }
        try database.execute("DELETE FROM tinta_installation_queue WHERE transaction_id=?",
            [.text(request.transaction.uuidString)])
        try database.execute("COMMIT"); committed = true
        return true
    }
    public func cancelQueuedTintaInstallation(_ transaction: UUID, owner: Data) throws {
        try database.execute("DELETE FROM tinta_installation_queue WHERE transaction_id=? AND owner=? AND phase='queued'",
            [.text(transaction.uuidString), .blob(owner)])
    }
    public func retainedTintaArtifactObjects() throws -> Set<ContentID> {
        let query = try database.query("SELECT manifest,payload FROM tinta_artifacts ORDER BY manifest")
        var references = Set<ContentID>(); references.reserveCapacity(16)
        while try query.next() {
            let id = try ContentID(query.text(0)), bytes = query.blob(1)
            guard Data(SHA256.hash(data: bytes)) == id.digest else { throw VaultError.integrity }
            let receipt = try TintaDerivedReceipt(decoding: bytes)
            references.insert(id)
            for digest in [receipt.packHash] + receipt.files.map(\.hash) {
                references.insert(try ContentID(digest.map { String(format: "%02x", $0) }.joined()))
            }
        }
        return references
    }
    @discardableResult
    public func releaseTintaArtifact(_ manifest: ContentID) throws -> Bool {
        let pending = try database.query("SELECT 1 FROM tinta_installation_queue WHERE manifest=?", [.text(manifest.hex)])
        guard try !pending.next() else { throw StoreError.conflictingJob }
        try database.execute("DELETE FROM tinta_artifacts WHERE manifest=?", [.text(manifest.hex)])
        return database.changedRows != 0
    }
    public func tintaDerivedInstallation(content: ContentID, vault: ContentVault, studyDay: UInt16,
                                         storageGeneration: Data, snapshotIdentity: Data,
                                         revision: UInt64) async throws -> TintaDerivedInstallation {
        try await buildTintaDerivedInstallation(content: content) { pack, details, journal in
            try await vault.tintaDerivedInstallation(content: content, course: pack.logicalIdentity,
                journal: journal, studyDay: studyDay, storageGeneration: storageGeneration,
                snapshotIdentity: snapshotIdentity, revision: revision, expectedLength: pack.length, expectedDetails: details)
        }
    }
    func buildTintaDerivedInstallation(content: ContentID,
        build: @Sendable (ContentManifest, CoursePackDetails, [JournalMutation]) async throws -> TintaDerivedInstallation
    ) async throws -> TintaDerivedInstallation {
        let pack = try courseManifest(content)
        guard let details = try coursePackDetails(content) else { throw StoreError.missingContent }
        let journal = try journalMutations()
        let installation = try await build(pack, details, journal)
        guard try courseManifest(content) == pack,
              try TintaJournalFrontier.digest(journalMutations()) == installation.frontierHash else {
            throw HistoryError.staleFrontier
        }
        return installation
    }
    public func replayTinta() throws -> TintaSnapshot {
        try TintaHistory.replay(journalMutations())
    }
    public func replayTinta(course: Data) throws -> TintaSnapshot {
        try TintaHistory.replay(journalMutations(), course: course)
    }
    private func journalMutations() throws -> [JournalMutation] {
        let query = try database.query("SELECT identity,resource,envelope,body FROM sync_events")
        var mutations: [JournalMutation] = []; mutations.reserveCapacity(256)
        while try query.next() { mutations.append(try decodeMutation(query)) }
        return mutations
    }
    @discardableResult
    public func importEvents(_ mutations: [JournalMutation]) throws -> Int {
        try Task.checkCancellation()
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let inserted = try importEventsInTransaction(mutations)
        try database.execute("COMMIT"); committed = true
        return inserted
    }
    public func importReaderJournal(_ mutations: [JournalMutation], reader: Data, generation: Data,
                                    frontier: Data, count: UInt32) throws -> Int {
        try Task.checkCancellation()
        guard reader.count == 16, reader.contains(where: { $0 != 0 }), generation.count == 16,
              generation.contains(where: { $0 != 0 }), mutations.count == Int(count),
              Set(mutations.map(\.event.identity)).count == mutations.count,
              try TintaJournalFrontier.digest(mutations) == frontier else { throw StoreError.invalidValue }
        for mutation in mutations { try mutation.validateReaderBody() }
        try Task.checkCancellation()
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let inserted = try importEventsInTransaction(mutations)
        try database.execute("""
            INSERT INTO reader_journal_baselines(reader,generation,frontier,event_count) VALUES(?,?,?,?)
            ON CONFLICT(reader,generation) DO UPDATE SET frontier=excluded.frontier,event_count=excluded.event_count
            """, [.blob(reader), .blob(generation), .blob(frontier), .integer(Int64(count))])
        try database.execute("COMMIT"); committed = true
        return inserted
    }
    public func readerJournalBaseline(reader: Data, generation: Data) throws -> ReaderJournalBaseline? {
        guard reader.count == 16, reader.contains(where: { $0 != 0 }), generation.count == 16,
              generation.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
        let query = try database.query("SELECT frontier,event_count FROM reader_journal_baselines WHERE reader=? AND generation=?",
                                       [.blob(reader), .blob(generation)])
        guard try query.next() else { return nil }
        let frontier = query.blob(0), count = query.integer(1)
        guard frontier.count == 32, frontier.contains(where: { $0 != 0 }), (0 ... Int64(UInt32.max)).contains(count) else {
            throw StoreError.invalidValue
        }
        return ReaderJournalBaseline(reader: reader, generation: generation, frontier: frontier, count: UInt32(count))
    }
    private func importEventsInTransaction(_ mutations: [JournalMutation]) throws -> Int {
        var courseClaims: [Data: Data] = [:]
        courseClaims.reserveCapacity(mutations.count)
        for mutation in mutations where mutation.event.kind.rawValue >= SyncEventKind.review.rawValue {
            let course = try TintaBody(mutation: mutation).subject.course
            let resource = mutation.event.resource
            if let previous = courseClaims[resource] {
                guard previous == course else { throw StoreError.invalidValue }
            } else {
                try requireCourseAssociation(resource: resource, course: course)
                courseClaims[resource] = course
            }
        }
        var inserted = 0
        for mutation in mutations {
            let event = mutation.event
            if let existing = try storedEvent(event.identity) {
                guard existing == mutation else { throw HistoryError.equivocation(event.identity) }
                continue
            }
            try database.execute("INSERT INTO sync_events(identity,resource,envelope,body) VALUES(?,?,?,?)",
                                 [.blob(event.identity.storageKey), .blob(event.resource), .blob(event.bytes), .blob(mutation.body)])
            inserted += 1
        }
        return inserted
    }
    public func storedEvent(_ identity: EventIdentity) throws -> JournalMutation? {
        let query = try database.query("SELECT identity,resource,envelope,body FROM sync_events WHERE identity=?", [.blob(identity.storageKey)])
        guard try query.next() else { return nil }
        return try decodeMutation(query)
    }
    public func syncEvents(resource: Data? = nil) throws -> [SyncEvent] {
        if let resource, resource.count != 32 { throw StoreError.invalidValue }
        let query = try database.query("SELECT identity,resource,envelope,body FROM sync_events" + (resource == nil ? "" : " WHERE resource=?"),
                                       resource.map { [.blob($0)] } ?? [])
        var events: [SyncEvent] = []; events.reserveCapacity(256)
        while try query.next() { events.append(try decodeMutation(query).event) }
        return events
    }
    // Identity byte order is a scan cursor, not causal or scheduler order. Restart scans to find late arrivals.
    public func syncMutations(after identity: EventIdentity? = nil, limit: Int = 128,
                              resource: Data? = nil) throws -> [JournalMutation] {
        guard (1...250).contains(limit), resource == nil || resource?.count == 32 else { throw StoreError.invalidValue }
        let cursor = identity?.storageKey ?? Data()
        let query = try database.query("""
            SELECT identity,resource,envelope,body FROM sync_events
            WHERE identity>? \(resource == nil ? "" : "AND resource=?") ORDER BY identity LIMIT ?
            """, resource.map { [.blob(cursor), .blob($0), .integer(Int64(limit))] }
                ?? [.blob(cursor), .integer(Int64(limit))])
        var mutations: [JournalMutation] = []; mutations.reserveCapacity(limit)
        while try query.next() { mutations.append(try decodeMutation(query)) }
        return mutations
    }
    private func cloudAccountKey(_ account: String) throws -> Data {
        guard !account.isEmpty, account.utf8.count <= 1024, !account.utf8.contains(0) else { throw StoreError.invalidValue }
        return Data(SHA256.hash(data: Data(account.utf8)))
    }
    private func cloudFingerprint(_ mutation: JournalMutation) -> Data {
        var hash = SHA256(); hash.update(data: mutation.event.bytes); hash.update(data: mutation.body)
        return Data(hash.finalize())
    }
    public func pendingCloudMutations(_ page: [JournalMutation], account: String) throws -> [JournalMutation] {
        guard page.count <= 250 else { throw StoreError.invalidValue }
        let key = try cloudAccountKey(account)
        var pending: [JournalMutation] = []; pending.reserveCapacity(page.count)
        for mutation in page {
            let query = try database.query("SELECT fingerprint FROM cloud_journal_receipts WHERE account=? AND identity=?",
                                           [.blob(key), .blob(mutation.event.identity.storageKey)])
            if try query.next() {
                guard query.blob(0) == cloudFingerprint(mutation) else { throw HistoryError.equivocation(mutation.event.identity) }
            } else { pending.append(mutation) }
        }
        return pending
    }
    public func acknowledgeCloudMutations(_ mutations: [JournalMutation], account: String) throws {
        guard mutations.count <= 250 else { throw StoreError.invalidValue }
        let key = try cloudAccountKey(account)
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        for mutation in mutations {
            guard try storedEvent(mutation.event.identity) == mutation else { throw StoreError.invalidValue }
            let fingerprint = cloudFingerprint(mutation)
            let query = try database.query("SELECT fingerprint FROM cloud_journal_receipts WHERE account=? AND identity=?",
                                           [.blob(key), .blob(mutation.event.identity.storageKey)])
            if try query.next() {
                guard query.blob(0) == fingerprint else { throw HistoryError.equivocation(mutation.event.identity) }
            } else {
                try database.execute("INSERT INTO cloud_journal_receipts(account,identity,fingerprint) VALUES(?,?,?)",
                                     [.blob(key), .blob(mutation.event.identity.storageKey), .blob(fingerprint)])
            }
        }
        try database.execute("COMMIT"); committed = true
    }
    private func decodeMutation(_ query: Statement) throws -> JournalMutation {
        let event = try SyncEvent(decoding: query.blob(2))
        guard event.identity.storageKey == query.blob(0), event.resource == query.blob(1) else { throw StoreError.invalidValue }
        return try JournalMutation(event: event, body: query.blob(3))
    }
    private func encodeStrings(_ values: [String]) throws -> String {
        String(decoding: try JSONEncoder().encode(values), as: UTF8.self)
    }
    private func decodeStrings(_ value: String) throws -> [String] {
        do { return try JSONDecoder().decode([String].self, from: Data(value.utf8)) }
        catch { throw StoreError.invalidValue }
    }
    @discardableResult
    public func confirmCourseSwitch(_ id: UUID, inventory: ReaderInventory) throws -> CourseSwitchRequest {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let consent = try confirmCourseSwitchInTransaction(id, inventory: inventory)
        try database.execute("COMMIT"); committed = true
        return consent
    }
    private func confirmCourseSwitchInTransaction(_ id: UUID, inventory: ReaderInventory) throws -> CourseSwitchRequest {
        guard let job = try job(id), inventory.complete, inventory.reader == job.reader,
              inventory.generation == job.storageGeneration,
              job.phase == .queued || job.phase == .paused,
              try courseBaselineConfirmation(id) == nil else { throw StoreError.invalidTransition }
        let courses = inventory.contents.filter { $0.kind == .course }
        guard courses.count <= 1, let old = inventory.boundCourse else { throw CourseTransferAdmissionError.multipleActiveCourses }
        let next = try courseManifest(job.content)
        guard old.length > 0, old.formatVersion > 0 else { throw StoreError.invalidValue }
        let consent = try CourseSwitchRequest(generation: job.storageGeneration,
            transaction: withUnsafeBytes(of: id.uuid) { Data($0) }, previousCourse: old.logicalIdentity,
            nextCourse: next.logicalIdentity, previousHash: old.content.digest, nextHash: next.content.digest)
        try database.execute("INSERT OR IGNORE INTO course_switch_confirmations(job,payload) VALUES(?,?)",
            [.text(id.uuidString), .blob(consent.bytes)])
        guard try courseSwitchConfirmation(id) == consent else { throw StoreError.conflictingJob }
        return consent
    }
    public func queueCourseSwitch(content candidate: ContentID, inventory: ReaderInventory, installation: Data) throws -> TransferJob {
        guard installation.count == 16, installation.contains(where: { $0 != 0 }), inventory.complete else { throw StoreError.invalidValue }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try !isLibraryContentDeleted(candidate) else { throw StoreError.invalidTransition }
        var existing: TransferJob?
        let pending = try database.query("""
            SELECT jobs.id,jobs.reader,jobs.generation,jobs.installation,jobs.content,jobs.offset,jobs.phase
            FROM jobs JOIN content ON jobs.content=content.hash
            WHERE jobs.reader=? AND content.kind=2 AND jobs.phase NOT IN ('completed','aborted')
            """, [.blob(inventory.reader)])
        while try pending.next() {
            let job = try decodeJob(pending)
            guard existing == nil, job.content == candidate, job.storageGeneration == inventory.generation,
                  job.installation == installation, job.phase == .queued || job.phase == .paused,
                  try !hasTransferAbort(job.id) else { throw StoreError.conflictingJob }
            existing = job
        }
        let id = existing?.id ?? UUID()
        if existing == nil {
            _ = try courseManifest(candidate)
            try database.execute("INSERT INTO jobs(id,reader,generation,installation,content,offset,phase) VALUES(?,?,?,?,?,0,'queued')",
                [.text(id.uuidString), .blob(inventory.reader), .blob(inventory.generation), .blob(installation), .text(candidate.hex)])
        }
        _ = try confirmCourseSwitchInTransaction(id, inventory: inventory)
        try database.execute("UPDATE reader_selections SET selected=0 WHERE reader=? AND content IN (SELECT hash FROM content WHERE kind=2) AND content!=? AND selected=1",
            [.blob(inventory.reader), .text(candidate.hex)])
        try database.execute("INSERT INTO reader_selections(reader,content,selected) VALUES(?,?,1) ON CONFLICT(reader,content) DO UPDATE SET selected=1 WHERE selected!=1",
            [.blob(inventory.reader), .text(candidate.hex)])
        guard let queued = try job(id) else { throw StoreError.missingJob }
        try database.execute("COMMIT"); committed = true
        return queued
    }
    public func courseSwitchConfirmation(_ id: UUID) throws -> CourseSwitchRequest? {
        let query = try database.query("SELECT payload FROM course_switch_confirmations WHERE job=?", [.text(id.uuidString)])
        guard try query.next() else { return nil }
        let consent = try CourseSwitchRequest(decoding: query.blob(0))
        guard let job = try job(id), consent.generation == job.storageGeneration,
              consent.transaction == withUnsafeBytes(of: id.uuid, { Data($0) }),
              consent.nextHash == job.content.digest,
              try courseIdentity(job.content) == consent.nextCourse else { throw StoreError.conflictingJob }
        return consent
    }
    @discardableResult
    public func confirmCourseBaselineImport(_ id: UUID, review: CourseBaselineReview) throws -> CourseBaselineImportRequest {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let request = try confirmCourseBaselineImportInTransaction(id, review: review)
        try database.execute("COMMIT"); committed = true
        return request
    }
    private func confirmCourseBaselineImportInTransaction(_ id: UUID, review: CourseBaselineReview) throws -> CourseBaselineImportRequest {
        guard let job = try job(id), job.phase == .queued || job.phase == .paused,
              job.durableOffset == 0, review.reader == job.reader,
              review.generation == job.storageGeneration, try !hasTransferAbort(id),
              try retainedTransferDeclaration(id) == nil,
              try !isLibraryContentDeleted(job.content), try courseSwitchConfirmation(id) == nil else {
            throw StoreError.invalidTransition
        }
        let request = try CourseBaselineImportRequest(generation: job.storageGeneration, owner: job.installation,
            transaction: withUnsafeBytes(of: id.uuid) { Data($0) }, manifest: courseManifest(job.content),
            reviewHash: review.hash)
        guard review.matches(request, reader: job.reader) else { throw StoreError.conflictingJob }
        try database.execute("INSERT OR IGNORE INTO course_baseline_confirmations(job,payload,review) VALUES(?,?,?)",
            [.text(id.uuidString), .blob(request.encoded), .blob(review.encoded)])
        guard try courseBaselineConfirmation(id) == request else { throw StoreError.conflictingJob }
        return request
    }
    public func queueCourseBaselineImport(content: ContentID, review: CourseBaselineReview,
                                          installation: Data) throws -> TransferJob {
        guard installation.count == 16, installation.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try !isLibraryContentDeleted(content),
              try !hasPendingRemoval(reader: review.reader, generation: review.generation, content: content) else {
            throw StoreError.invalidTransition
        }
        var existing: TransferJob?
        let pending = try database.query("""
            SELECT jobs.id,jobs.reader,jobs.generation,jobs.installation,jobs.content,jobs.offset,jobs.phase
            FROM jobs JOIN content ON jobs.content=content.hash
            WHERE jobs.reader=? AND content.kind=2 AND jobs.phase NOT IN ('completed','aborted')
            """, [.blob(review.reader)])
        while try pending.next() {
            let job = try decodeJob(pending)
            guard existing == nil, job.content == content, job.storageGeneration == review.generation,
                  job.installation == installation, job.durableOffset == 0,
                  job.phase == .queued || job.phase == .paused, try !hasTransferAbort(job.id) else {
                throw StoreError.conflictingJob
            }
            existing = job
        }
        let id = existing?.id ?? UUID()
        if existing == nil {
            _ = try courseManifest(content)
            try database.execute("INSERT INTO jobs(id,reader,generation,installation,content,offset,phase) VALUES(?,?,?,?,?,0,'queued')",
                [.text(id.uuidString), .blob(review.reader), .blob(review.generation), .blob(installation), .text(content.hex)])
        }
        _ = try confirmCourseBaselineImportInTransaction(id, review: review)
        guard let job = try job(id) else { throw StoreError.missingJob }
        try database.execute("COMMIT"); committed = true
        return job
    }
    public func courseBaselineConfirmation(_ id: UUID) throws -> CourseBaselineImportRequest? {
        let query = try database.query("SELECT payload,review FROM course_baseline_confirmations WHERE job=?",
            [.text(id.uuidString)])
        guard try query.next() else { return nil }
        let request = try CourseBaselineImportRequest(decoding: query.blob(0))
        let review = try CourseBaselineReview(decoding: query.blob(1))
        guard let job = try job(id), request.generation == job.storageGeneration,
              request.owner == job.installation, request.transaction == withUnsafeBytes(of: id.uuid, { Data($0) }),
              try request.manifest == storedCourseManifest(job.content), review.matches(request, reader: job.reader) else {
            throw StoreError.conflictingJob
        }
        return request
    }
    public func enqueue(content: ContentID, reader: Data, storageGeneration: Data, installation: Data,
                        transaction: UUID = UUID()) throws -> TransferJob {
        guard reader.count == 16, storageGeneration.count == 16, installation.count == 16,
              reader.contains(where: { $0 != 0 }), storageGeneration.contains(where: { $0 != 0 }),
              installation.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try self.content(content) != nil else { throw StoreError.missingContent }
        guard try !isLibraryContentDeleted(content) else { throw StoreError.invalidTransition }
        guard try removalJob(transaction) == nil else { throw StoreError.conflictingJob }
        guard try readerImportJob(transaction) == nil else { throw StoreError.conflictingJob }
        if let existing = try job(transaction) {
            guard existing.reader == reader, existing.storageGeneration == storageGeneration,
                  existing.installation == installation, existing.content == content else { throw StoreError.conflictingJob }
            try database.execute("COMMIT"); committed = true; return existing
        }
        guard try !hasPendingRemoval(reader: reader, generation: storageGeneration, content: content) else {
            throw StoreError.conflictingJob
        }
        try database.execute("""
            INSERT INTO jobs(id,reader,generation,installation,content,offset,phase) VALUES(?,?,?,?,?,0,'queued')
            """, [.text(transaction.uuidString), .blob(reader), .blob(storageGeneration), .blob(installation), .text(content.hex)])
        guard let created = try job(transaction) else { throw StoreError.missingJob }
        try database.execute("COMMIT"); committed = true; return created
    }
    public func enqueueFirmware(content: ContentID, reader: Data, storageGeneration: Data,
                                installation: Data) throws -> TransferJob {
        try enqueueContent(content: content, reader: reader, storageGeneration: storageGeneration,
                           installation: installation, firmware: true)
    }
    public func enqueueSelectedContent(content: ContentID, reader: Data, storageGeneration: Data,
                                       installation: Data) throws -> TransferJob {
        try enqueueContent(content: content, reader: reader, storageGeneration: storageGeneration,
                           installation: installation, firmware: false)
    }
    private func enqueueContent(content: ContentID, reader: Data, storageGeneration: Data,
                                installation: Data, firmware: Bool) throws -> TransferJob {
        guard [reader, storageGeneration, installation].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }) else {
            throw StoreError.invalidValue
        }
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard let asset = try self.content(content) else { throw StoreError.missingContent }
        guard (asset.kind == .firmware) == firmware else { throw StoreError.invalidValue }
        if firmware { guard try firmwareCompatibility(content) != nil else { throw StoreError.invalidValue } }
        guard try !isLibraryContentDeleted(content) else { throw StoreError.invalidTransition }
        if !firmware {
            let choice = try database.query("SELECT selected FROM reader_selections WHERE reader=? AND content=?",
                                            [.blob(reader), .text(content.hex)])
            guard try choice.next(), choice.integer(0) == 1 else { throw StoreError.invalidTransition }
        }
        var existing: TransferJob?
        do {
            let query = try database.query("""
                SELECT id,reader,generation,installation,content,offset,phase FROM jobs
                WHERE reader=? AND generation=? AND content=? AND phase NOT IN ('completed','aborted')
                """, [.blob(reader), .blob(storageGeneration), .text(content.hex)])
            if try query.next() {
                let candidate = try decodeJob(query)
                guard candidate.installation == installation, try !query.next() else { throw StoreError.conflictingJob }
                guard candidate.phase != .failed, try !hasTransferAbort(candidate.id) else { throw StoreError.invalidTransition }
                existing = candidate
            }
        }
        if let existing {
            try database.execute("COMMIT"); committed = true
            return existing
        }
        let transaction = UUID()
        guard try removalJob(transaction) == nil else { throw StoreError.conflictingJob }
        guard try readerImportJob(transaction) == nil else { throw StoreError.conflictingJob }
        guard try !hasPendingRemoval(reader: reader, generation: storageGeneration, content: content) else {
            throw StoreError.conflictingJob
        }
        try database.execute("""
            INSERT INTO jobs(id,reader,generation,installation,content,offset,phase) VALUES(?,?,?,?,?,0,'queued')
            """, [.text(transaction.uuidString), .blob(reader), .blob(storageGeneration), .blob(installation), .text(content.hex)])
        guard let created = try job(transaction) else { throw StoreError.missingJob }
        try database.execute("COMMIT"); committed = true
        return created
    }
    public func prepareTransferDeclaration(_ id: UUID, verifiedLength: UInt64) throws -> TransferDeclaration {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard try courseBaselineConfirmation(id) == nil else { throw StoreError.invalidTransition }
        guard let job = try job(id) else { throw StoreError.missingJob }
        guard let content = try content(job.content), try !isLibraryContentDeleted(job.content) else {
            throw StoreError.missingContent
        }
        guard content.length == verifiedLength else { throw VaultError.integrity }
        guard job.phase != .aborted, try !hasTransferAbort(id) else { throw StoreError.invalidTransition }
        let manifest: ContentManifest
        switch content.kind {
        case .course: manifest = try courseManifest(job.content)
        case .firmware:
            guard let metadata = try firmwareCompatibility(job.content), metadata.length == content.length,
                  metadata.sha256 == job.content.digest else { throw FirmwareTransferAdmissionError.missingMetadata }
            manifest = try ContentManifest(content: job.content, kind: .firmware, length: content.length,
                formatVersion: 1, logicalIdentity: Data(count: 16))
        case .font:
            let plan = try FontTransferPlan(content: content)
            let retained = try database.query("SELECT destination FROM job_font_destinations WHERE job=?", [.text(id.uuidString)])
            if try retained.next() {
                guard retained.text(0) == plan.destination else { throw StoreError.conflictingJob }
            } else {
                guard job.durableOffset == 0, job.phase == .queued || job.phase == .paused else { throw StoreError.invalidTransition }
                try database.execute("INSERT INTO job_font_destinations(job,destination) VALUES(?,?)",
                    [.text(id.uuidString), .text(plan.destination)])
            }
            manifest = try ContentManifest(content: job.content, kind: .font, length: content.length,
                formatVersion: plan.formatVersion, logicalIdentity: Data(count: 16))
        case .dictionary:
            let plan = try DictionaryTransferPlan(content: content)
            manifest = try ContentManifest(content: job.content, kind: .dictionary, length: content.length,
                formatVersion: plan.formatVersion, logicalIdentity: Data(count: 16))
        case .epub:
            manifest = try ContentManifest(content: job.content, kind: .epub, length: content.length,
                                           formatVersion: 0, logicalIdentity: Data(count: 16))
        default: throw TransferRunnerError.unsupportedContent
        }
        let transaction = withUnsafeBytes(of: job.id.uuid) { Data($0) }
        let state = try TransferState(transaction: transaction, owner: job.installation,
            storageGeneration: job.storageGeneration, contentHash: job.content.digest, length: content.length)
        let declaration = try TransferDeclaration(manifest: manifest, state: state)
        if let retained = try retainedTransferDeclaration(id) {
            guard retained == declaration else { throw StoreError.conflictingJob }
        } else {
            guard job.durableOffset == 0, job.phase == .queued || job.phase == .paused,
                  try !hasTransferAbort(id) else { throw StoreError.invalidTransition }
            try database.execute("INSERT INTO job_transfer_declarations(job,payload) VALUES(?,?)",
                                 [.text(id.uuidString), .blob(declaration.encoded)])
        }
        try database.execute("COMMIT"); committed = true
        return declaration
    }
    public func prepareCourseBaselineDeclaration(_ id: UUID, verifiedLength: UInt64) throws -> TransferDeclaration {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard let job = try job(id), let consent = try courseBaselineConfirmation(id),
              let content = try content(job.content), content.kind == .course,
              job.phase != .aborted, try job.phase == .committing || !isLibraryContentDeleted(job.content),
              try !hasTransferAbort(id) else { throw StoreError.invalidTransition }
        guard content.length == verifiedLength, consent.manifest.length == verifiedLength else {
            throw VaultError.integrity
        }
        let transaction = withUnsafeBytes(of: job.id.uuid) { Data($0) }
        let state = try TransferState(transaction: transaction, owner: job.installation,
            storageGeneration: job.storageGeneration, contentHash: job.content.digest, length: verifiedLength)
        let declaration = try TransferDeclaration(manifest: storedCourseManifest(job.content), state: state)
        guard consent.matches(generation: job.storageGeneration, owner: job.installation,
                              reviewed: consent.reviewHash, transfer: declaration) else {
            throw StoreError.conflictingJob
        }
        if let retained = try retainedTransferDeclaration(id) {
            guard retained == declaration else { throw StoreError.conflictingJob }
        } else {
            guard job.durableOffset == 0, job.phase == .queued || job.phase == .paused else {
                throw StoreError.invalidTransition
            }
            try database.execute("INSERT INTO job_transfer_declarations(job,payload) VALUES(?,?)",
                                 [.text(id.uuidString), .blob(declaration.encoded)])
        }
        try database.execute("COMMIT"); committed = true
        return declaration
    }
    public func fontTransferPlan(_ id: UUID) throws -> FontTransferPlan {
        guard let job = try job(id), let content = try content(job.content) else { throw StoreError.missingJob }
        let plan = try FontTransferPlan(content: content)
        let query = try database.query("SELECT destination FROM job_font_destinations WHERE job=?", [.text(id.uuidString)])
        guard try query.next(), query.text(0) == plan.destination,
              let declaration = try retainedTransferDeclaration(id), declaration.manifest.kind == .font,
              declaration.manifest.formatVersion == plan.formatVersion else { throw StoreError.conflictingJob }
        return plan
    }
    public func stagedFirmwareJobs(reader: Data, generation: Data, installation: Data) throws -> [TransferJob] {
        let query = try database.query("""
            SELECT j.id,j.reader,j.generation,j.installation,j.content,j.offset,j.phase
            FROM jobs j JOIN content c ON c.hash=j.content
            WHERE j.reader=? AND j.generation=? AND j.installation=? AND j.phase='completed' AND c.kind=5
            ORDER BY j.rowid DESC
            """, [.blob(reader), .blob(generation), .blob(installation)])
        var results: [TransferJob] = []
        while try query.next() {
            let job = try decodeJob(query)
            _ = try firmwareStagingReceipt(job.id)
            results.append(job)
        }
        return results
    }
    public func firmwareInstallations(reader: Data, installation: Data) throws -> [FirmwareInstallation] {
        let query = try database.query("""
            SELECT f.job FROM firmware_installations f JOIN jobs j ON j.id=f.job
            WHERE j.reader=? AND j.installation=? ORDER BY j.rowid DESC
            """, [.blob(reader), .blob(installation)])
        var results: [FirmwareInstallation] = []
        while try query.next() {
            guard let id = UUID(uuidString: query.text(0)), let saved = try firmwareInstallation(id) else {
                throw StoreError.invalidValue
            }
            results.append(saved)
        }
        return results
    }
    public func prepareFirmwareInstallation(_ id: UUID, device: DeviceDescriptor,
                                            info: FirmwareReaderInfo, installation: Data) throws -> FirmwareInstallation {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        let receipt = try firmwareStagingReceipt(id)
        guard receipt.reader == device.identity, receipt.generation == device.storageGeneration,
              receipt.installation == installation else { throw StoreError.conflictingJob }
        guard let metadata = try firmwareCompatibility(receipt.image) else {
            throw FirmwareTransferAdmissionError.missingMetadata
        }
        let request = try FirmwareInstallRequest(receipt: receipt, metadata: metadata, device: device, info: info)
        do {
            let conflicts = try database.query("""
                SELECT f.job FROM firmware_installations f JOIN jobs j ON j.id=f.job
                WHERE j.reader=? AND f.verified=0 AND f.job<>?
                """, [.blob(receipt.reader), .text(id.uuidString)])
            guard try !conflicts.next() else { throw StoreError.conflictingJob }
        }
        if let saved = try firmwareInstallation(id) {
            guard saved.request == request else { throw StoreError.conflictingJob }
        } else {
            try database.execute("INSERT INTO firmware_installations(job,payload,verified) VALUES(?,?,0)",
                [.text(id.uuidString), .blob(request.bytes)])
        }
        guard let prepared = try firmwareInstallation(id) else { throw StoreError.missingJob }
        try database.execute("COMMIT"); committed = true
        return prepared
    }
    public func firmwareInstallation(_ id: UUID) throws -> FirmwareInstallation? {
        let query = try database.query("SELECT payload,verified FROM firmware_installations WHERE job=?", [.text(id.uuidString)])
        guard try query.next() else { return nil }
        let request = try FirmwareInstallRequest(decoding: query.blob(0))
        let receipt = try firmwareStagingReceipt(id)
        guard request.bytes.subdata(in: 4..<20) == receipt.generation,
              request.bytes.subdata(in: 20..<36) == withUnsafeBytes(of: id.uuid, { Data($0) }),
              request.bytes.subdata(in: 36..<68) == receipt.image.digest else { throw StoreError.conflictingJob }
        var length = ByteReader(request.bytes.subdata(in: 68..<76))
        guard try length.number(8) == receipt.length else { throw StoreError.conflictingJob }
        return FirmwareInstallation(receipt: receipt, request: request, bootVerified: query.integer(1) == 1)
    }
    /// Supply discovery from the newly authenticated connection, never a transfer acknowledgement.
    public func verifyFirmwareInstallation(_ id: UUID, device: DeviceDescriptor, installation: Data) throws -> Bool {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard let saved = try firmwareInstallation(id) else { throw StoreError.missingJob }
        guard device.identity == saved.receipt.reader, device.storageGeneration == saved.receipt.generation,
              installation == saved.receipt.installation, device.board.rawValue == saved.request.bytes[91] else {
            throw StoreError.conflictingJob
        }
        guard device.runningBuild == saved.receipt.image.digest else {
            try database.execute("COMMIT"); committed = true
            return false
        }
        try database.execute("UPDATE firmware_installations SET verified=1 WHERE job=?", [.text(id.uuidString)])
        try database.execute("COMMIT"); committed = true
        return true
    }
    public func firmwareStagingReceipt(_ id: UUID) throws -> FirmwareStagingReceipt {
        guard let job = try job(id), job.phase == .completed,
              let declaration = try retainedTransferDeclaration(id), declaration.manifest.kind == .firmware,
              declaration.manifest.formatVersion == 1, declaration.manifest.length == job.durableOffset,
              declaration.manifest.logicalIdentity == Data(count: 16),
              let metadata = try firmwareCompatibility(job.content), metadata.length == job.durableOffset else {
            throw StoreError.invalidTransition
        }
        return FirmwareStagingReceipt(job: job, length: declaration.manifest.length)
    }
    public func retainedTransferDeclaration(_ id: UUID) throws -> TransferDeclaration? {
        let query = try database.query("SELECT payload FROM job_transfer_declarations WHERE job=?", [.text(id.uuidString)])
        guard try query.next() else { return nil }
        let declaration = try TransferDeclaration(decoding: query.blob(0))
        guard let job = try job(id), declaration.state.transaction == withUnsafeBytes(of: id.uuid, { Data($0) }),
              declaration.state.owner == job.installation, declaration.state.storageGeneration == job.storageGeneration,
              declaration.manifest.content == job.content else { throw StoreError.conflictingJob }
        return declaration
    }
    public func job(_ id: UUID) throws -> TransferJob? {
        let query = try database.query("SELECT id,reader,generation,installation,content,offset,phase FROM jobs WHERE id=?", [.text(id.uuidString)])
        guard try query.next() else { return nil }
        return try decodeJob(query)
    }
    public func pendingJobs() throws -> [TransferJob] {
        let query = try database.query("SELECT id,reader,generation,installation,content,offset,phase FROM jobs WHERE phase NOT IN ('completed','aborted') ORDER BY rowid")
        var jobs: [TransferJob] = []
        jobs.reserveCapacity(32)
        while try query.next() { jobs.append(try decodeJob(query)) }
        return jobs
    }
    public func hasTransferAbort(_ id: UUID) throws -> Bool {
        let query = try database.query("SELECT 1 FROM transfer_aborts WHERE job=?", [.text(id.uuidString)])
        return try query.next()
    }
    public func requestTransferAbort(_ id: UUID) throws {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard let job = try job(id) else { throw StoreError.missingJob }
        guard job.phase != .committing, job.phase != .completed else { throw StoreError.invalidTransition }
        try database.execute("INSERT INTO transfer_aborts(job) VALUES(?) ON CONFLICT(job) DO NOTHING", [.text(id.uuidString)])
        try database.execute("COMMIT"); committed = true
    }
    public func isReaderContentSelected(reader: Data, content: ContentID) throws -> Bool {
        guard reader.count == 16, reader.contains(where: { $0 != 0 }) else { throw StoreError.invalidValue }
        let query = try database.query("SELECT selected FROM reader_selections WHERE reader=? AND content=?",
                                       [.blob(reader), .text(content.hex)])
        return try query.next() && query.integer(0) == 1
    }
    public func commitSelectedTransfer(_ id: UUID) throws {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard let job = try job(id), let asset = try content(job.content) else { throw StoreError.missingJob }
        guard job.phase != .aborted, job.phase != .completed, job.durableOffset == asset.length, asset.kind != .firmware,
              try courseBaselineConfirmation(id) == nil,
              try isReaderContentSelected(reader: job.reader, content: job.content) else { throw StoreError.invalidTransition }
        try checkpoint(id, offset: asset.length, phase: .committing)
        try database.execute("COMMIT"); committed = true
    }
    public func commitCourseBaselineTransfer(_ id: UUID) throws {
        try database.execute("BEGIN IMMEDIATE")
        var committed = false
        defer { if !committed { try? database.execute("ROLLBACK") } }
        guard let job = try job(id), let asset = try content(job.content) else { throw StoreError.missingJob }
        guard asset.kind == .course, job.phase != .aborted, job.phase != .completed,
              job.durableOffset == asset.length, try courseBaselineConfirmation(id) != nil,
              try job.phase == .committing || !isLibraryContentDeleted(job.content), try !hasTransferAbort(id) else {
            throw StoreError.invalidTransition
        }
        try checkpoint(id, offset: asset.length, phase: .committing)
        try database.execute("COMMIT"); committed = true
    }
    public func checkpoint(_ id: UUID, offset: UInt64, phase: JobPhase) throws {
        guard offset <= UInt64(Int64.max) else { throw StoreError.invalidValue }
        if phase == .transferring || phase == .committing {
            guard try !hasTransferAbort(id) else { throw StoreError.invalidTransition }
        }
        guard let current = try job(id) else { throw StoreError.missingJob }
        guard let asset = try content(current.content), offset <= asset.length else { throw StoreError.invalidValue }
        if current.phase == .completed || current.phase == .aborted {
            guard current.phase == phase, current.durableOffset == offset else { throw StoreError.invalidTransition }
            return
        }
        guard phase != .queued,
              (phase != .completed && phase != .committing) || offset == asset.length,
              phase != .completed || current.phase == .committing,
              phase != .aborted || current.phase != .committing else { throw StoreError.invalidTransition }
        try database.execute("UPDATE jobs SET offset=?,phase=? WHERE id=? AND offset=? AND phase=?",
                             [.integer(Int64(offset)), .text(phase.rawValue), .text(id.uuidString),
                              .integer(Int64(current.durableOffset)), .text(current.phase.rawValue)])
        guard database.changedRows == 1 else { throw StoreError.conflictingJob }
    }
    private func decodeJob(_ query: Statement) throws -> TransferJob {
        guard let id = UUID(uuidString: query.text(0)), let phase = JobPhase(rawValue: query.text(6)),
              query.integer(5) >= 0 else { throw StoreError.invalidValue }
        let reader = query.blob(1), generation = query.blob(2), installation = query.blob(3)
        guard reader.count == 16, generation.count == 16, installation.count == 16 else { throw StoreError.invalidValue }
        return TransferJob(id: id, reader: reader, storageGeneration: generation, installation: installation,
                           content: try ContentID(query.text(4)), durableOffset: UInt64(query.integer(5)), phase: phase)
    }
}

private enum Binding { case text(String), blob(Data), integer(Int64) }
private final class Database {
    private var handle: OpaquePointer?
    init(url: URL) throws {
        var opened: OpaquePointer?
        let status = sqlite3_open_v2(url.path, &opened, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nil)
        guard status == SQLITE_OK, let opened else {
            let message = opened.map { String(cString: sqlite3_errmsg($0)) } ?? "SQLite open failed"
            if let opened { sqlite3_close_v2(opened) }
            throw StoreError.database(message)
        }
        handle = opened
        do {
            sqlite3_busy_timeout(handle, 5000)
            try execute("PRAGMA foreign_keys=ON")
            try execute("PRAGMA journal_mode=WAL")
            try execute("PRAGMA synchronous=FULL")
            try execute("BEGIN IMMEDIATE")
            var committed = false
            defer { if !committed { try? execute("ROLLBACK") } }
            let schema: Int64
            do {
                let version = try query("PRAGMA user_version")
                guard try version.next(), (0 ... 41).contains(version.integer(0)) else { throw StoreError.unsupportedSchema }
                schema = version.integer(0)
            }
            if schema == 0 {
                try execute("""
                CREATE TABLE IF NOT EXISTS content(hash TEXT PRIMARY KEY NOT NULL,kind INTEGER NOT NULL CHECK(kind BETWEEN 1 AND 5),
                    length INTEGER NOT NULL CHECK(length>=0),title TEXT NOT NULL,filename TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS jobs(id TEXT PRIMARY KEY NOT NULL,reader BLOB NOT NULL CHECK(length(reader)=16),
                    generation BLOB NOT NULL CHECK(length(generation)=16),installation BLOB NOT NULL CHECK(length(installation)=16),
                    content TEXT NOT NULL REFERENCES content(hash),offset INTEGER NOT NULL CHECK(offset>=0),
                    phase TEXT NOT NULL CHECK(phase IN ('queued','transferring','committing','paused','completed','failed','aborted')));
                """)
            }
            if schema < 2 {
                try execute("""
                ALTER TABLE content ADD COLUMN authors TEXT NOT NULL DEFAULT '[]';
                ALTER TABLE content ADD COLUMN identifiers TEXT NOT NULL DEFAULT '[]';
                ALTER TABLE content ADD COLUMN languages TEXT NOT NULL DEFAULT '[]';
                PRAGMA user_version=2;
                """)
            }
            if schema < 3 {
                try execute("""
                CREATE TABLE sync_events(identity BLOB PRIMARY KEY NOT NULL CHECK(length(identity)=32),
                    resource BLOB NOT NULL CHECK(length(resource)=32),
                    envelope BLOB NOT NULL CHECK(length(envelope) BETWEEN 165 AND 293),
                    body BLOB NOT NULL CHECK(length(body)<=65536));
                CREATE INDEX sync_events_resource ON sync_events(resource);
                PRAGMA user_version=3;
                """)
            }
            if schema < 4 {
                try execute("""
                CREATE TABLE local_origins(origin BLOB PRIMARY KEY NOT NULL CHECK(length(origin)=16),
                    epoch BLOB NOT NULL CHECK(length(epoch)=8),generation BLOB NOT NULL CHECK(length(generation)=16),
                    sequence BLOB NOT NULL CHECK(length(sequence)=8));
                PRAGMA user_version=4;
                """)
            }
            if schema < 5 {
                try execute("""
                CREATE TABLE legacy_backups(hash TEXT PRIMARY KEY NOT NULL CHECK(length(hash)=64),
                    reader BLOB NOT NULL CHECK(length(reader)=16),generation BLOB NOT NULL CHECK(length(generation)=16),
                    course BLOB NOT NULL CHECK(length(course)=16));
                CREATE INDEX legacy_backups_reader ON legacy_backups(reader,generation,course);
                PRAGMA user_version=5;
                """)
            }
            if schema < 6 {
                try execute("""
                CREATE TABLE legacy_migrations(backup TEXT PRIMARY KEY NOT NULL REFERENCES legacy_backups(hash),
                    origin BLOB NOT NULL CHECK(length(origin)=16),epoch BLOB NOT NULL CHECK(length(epoch)=8),
                    resource BLOB NOT NULL CHECK(length(resource)=32),configuration BLOB NOT NULL CHECK(length(configuration)=6),
                    UNIQUE(origin,epoch));
                PRAGMA user_version=6;
                """)
            }
            if schema < 7 {
                try execute("""
                CREATE TABLE legacy_installations(backup TEXT PRIMARY KEY NOT NULL REFERENCES legacy_migrations(backup),
                    digest BLOB NOT NULL CHECK(length(digest)=32));
                PRAGMA user_version=7;
                """)
            }
            if schema < 8 {
                try execute("""
                CREATE TABLE reader_selections(reader BLOB NOT NULL CHECK(length(reader)=16),
                    content TEXT NOT NULL REFERENCES content(hash),selected INTEGER NOT NULL CHECK(selected IN (0,1)),
                    PRIMARY KEY(reader,content));
                PRAGMA user_version=8;
                """)
            }
            if schema < 9 {
                try execute("""
                CREATE TABLE transfer_aborts(job TEXT PRIMARY KEY NOT NULL REFERENCES jobs(id));
                PRAGMA user_version=9;
                """)
            }
            if schema < 10 {
                try execute("""
                CREATE TABLE library_deletions(content TEXT PRIMARY KEY NOT NULL REFERENCES content(hash));
                PRAGMA user_version=10;
                """)
            }
            if schema < 11 {
                try execute("""
                CREATE TABLE course_packs(content TEXT PRIMARY KEY NOT NULL REFERENCES content(hash),
                    major INTEGER NOT NULL CHECK(major BETWEEN 0 AND 65535),
                    minor INTEGER NOT NULL CHECK(minor BETWEEN 0 AND 65535),
                    edition INTEGER NOT NULL CHECK(edition BETWEEN 0 AND 4294967295),locale TEXT NOT NULL);
                PRAGMA user_version=11;
                """)
            }
            if schema < 12 {
                try execute("""
                CREATE TABLE course_associations(content TEXT PRIMARY KEY NOT NULL REFERENCES course_packs(content),
                    identity BLOB NOT NULL CHECK(length(identity)=16 AND identity!=zeroblob(16)));
                PRAGMA user_version=12;
                """)
            }
            if schema < 13 {
                try execute("""
                CREATE TABLE readers(identity BLOB PRIMARY KEY NOT NULL CHECK(length(identity)=16 AND identity!=zeroblob(16)),
                    descriptor BLOB NOT NULL CHECK(length(descriptor)=74),last_connected INTEGER NOT NULL CHECK(last_connected>=0),
                    last_sync INTEGER CHECK(last_sync>=0));
                PRAGMA user_version=13;
                """)
            }
            if schema < 14 {
                try execute("""
                CREATE TABLE legacy_shared_backups(backup TEXT PRIMARY KEY NOT NULL REFERENCES legacy_backups(hash),
                    canonical TEXT NOT NULL REFERENCES legacy_installations(backup),
                    original_hash BLOB NOT NULL CHECK(length(original_hash)=32),
                    digest BLOB NOT NULL CHECK(length(digest)=32),CHECK(backup!=canonical));
                PRAGMA user_version=14;
                """)
            }
            if schema < 15 {
                try execute("""
                CREATE TABLE legacy_migration_drafts(backup TEXT PRIMARY KEY NOT NULL REFERENCES legacy_backups(hash),
                    payload BLOB NOT NULL CHECK(length(payload)<=1048576));
                PRAGMA user_version=15;
                """)
            }
            if schema < 16 {
                try execute("""
                CREATE TABLE cloud_journal_receipts(account BLOB NOT NULL CHECK(length(account)=32),
                    identity BLOB NOT NULL REFERENCES sync_events(identity) CHECK(length(identity)=32),
                    fingerprint BLOB NOT NULL CHECK(length(fingerprint)=32), PRIMARY KEY(account,identity));
                PRAGMA user_version=16;
                """)
            }
            if schema < 17 {
                try execute("""
                CREATE TABLE cloud_content_receipts(account BLOB NOT NULL CHECK(length(account)=32),
                    content TEXT NOT NULL REFERENCES content(hash) CHECK(length(content)=64),
                    kind INTEGER NOT NULL CHECK(kind BETWEEN 1 AND 4), length INTEGER NOT NULL CHECK(length>0),
                    PRIMARY KEY(account,content));
                PRAGMA user_version=17;
                """)
            }
            if schema < 18 {
                try execute("""
                CREATE TABLE cloud_course_bindings(content TEXT PRIMARY KEY NOT NULL CHECK(length(content)=64),
                    identity BLOB NOT NULL CHECK(length(identity)=16));
                PRAGMA user_version=18;
                """)
            }
            if schema < 19 {
                try execute("""
                CREATE TABLE cloud_course_receipts(account BLOB NOT NULL CHECK(length(account)=32),
                    content TEXT NOT NULL CHECK(length(content)=64), identity BLOB NOT NULL CHECK(length(identity)=16),
                    PRIMARY KEY(account,content));
                PRAGMA user_version=19;
                """)
            }
            if schema < 20 {
                try execute("""
                CREATE TABLE library_visibility_events(identity TEXT PRIMARY KEY NOT NULL CHECK(length(identity)=36),
                    content TEXT NOT NULL CHECK(length(content)=64), payload BLOB NOT NULL CHECK(length(payload)<=4096));
                CREATE INDEX library_visibility_content ON library_visibility_events(content);
                PRAGMA user_version=20;
                """)
            }
            if schema < 21 {
                try execute("""
                CREATE TABLE cloud_visibility_receipts(account BLOB NOT NULL CHECK(length(account)=32),
                    identity TEXT NOT NULL REFERENCES library_visibility_events(identity) CHECK(length(identity)=36),
                    fingerprint BLOB NOT NULL CHECK(length(fingerprint)=32), PRIMARY KEY(account,identity));
                PRAGMA user_version=21;
                """)
            }
            if schema < 22 {
                try execute("""
                CREATE TABLE job_transfer_declarations(job TEXT PRIMARY KEY NOT NULL REFERENCES jobs(id),
                    payload BLOB NOT NULL CHECK(length(payload)=162));
                PRAGMA user_version=22;
                """)
            }
            if schema < 23 {
                try execute("""
                CREATE TABLE tinta_artifacts(manifest TEXT PRIMARY KEY NOT NULL CHECK(length(manifest)=64),
                    payload BLOB NOT NULL CHECK(length(payload)=332));
                PRAGMA user_version=23;
                """)
            }
            if schema < 24 {
                try execute("""
                CREATE TABLE tinta_installation_queue(transaction_id TEXT PRIMARY KEY NOT NULL CHECK(length(transaction_id)=36),
                    manifest TEXT NOT NULL REFERENCES tinta_artifacts(manifest),
                    reader BLOB NOT NULL CHECK(length(reader)=16), generation BLOB NOT NULL CHECK(length(generation)=16),
                    owner BLOB NOT NULL CHECK(length(owner)=16), UNIQUE(reader,generation));
                PRAGMA user_version=24;
                """)
            }
            if schema < 25 {
                try execute("""
                ALTER TABLE tinta_installation_queue ADD COLUMN phase TEXT NOT NULL DEFAULT 'queued'
                    CHECK(phase IN ('queued','staging'));
                PRAGMA user_version=25;
                """)
            }
            if schema < 26 {
                try execute("""
                CREATE TABLE tinta_installation_queue_v26(transaction_id TEXT PRIMARY KEY NOT NULL CHECK(length(transaction_id)=36),
                    manifest TEXT NOT NULL REFERENCES tinta_artifacts(manifest),
                    reader BLOB NOT NULL CHECK(length(reader)=16), generation BLOB NOT NULL CHECK(length(generation)=16),
                    owner BLOB NOT NULL CHECK(length(owner)=16),
                    phase TEXT NOT NULL DEFAULT 'queued' CHECK(phase IN ('queued','staging','committing')),
                    UNIQUE(reader,generation));
                INSERT INTO tinta_installation_queue_v26 SELECT transaction_id,manifest,reader,generation,owner,phase FROM tinta_installation_queue;
                DROP TABLE tinta_installation_queue;
                ALTER TABLE tinta_installation_queue_v26 RENAME TO tinta_installation_queue;
                PRAGMA user_version=26;
                """)
            }
            if schema < 27 {
                try execute("""
                CREATE TABLE firmware_assets(content TEXT PRIMARY KEY NOT NULL REFERENCES content(hash),
                    metadata BLOB NOT NULL CHECK(length(metadata)>0 AND length(metadata)<=16384));
                PRAGMA user_version=27;
                """)
            }
            if schema < 28 {
                try execute("""
                CREATE TABLE reader_journal_baselines(reader BLOB NOT NULL CHECK(length(reader)=16),
                    generation BLOB NOT NULL CHECK(length(generation)=16),
                    frontier BLOB NOT NULL CHECK(length(frontier)=32),
                    event_count INTEGER NOT NULL CHECK(event_count>=0 AND event_count<=4294967295),
                    PRIMARY KEY(reader,generation));
                PRAGMA user_version=28;
                """)
            }
            if schema < 29 {
                try execute("""
                CREATE TABLE legacy_backup_jobs(reader BLOB NOT NULL CHECK(length(reader)=16),
                    generation BLOB NOT NULL CHECK(length(generation)=16),
                    course BLOB NOT NULL CHECK(length(course)=16),
                    transaction_id BLOB NOT NULL CHECK(length(transaction_id)=16),
                    completed INTEGER NOT NULL CHECK(completed IN (0,1)),
                    backup TEXT NOT NULL CHECK((completed=0 AND backup='') OR (completed=1 AND length(backup)=64)),
                    PRIMARY KEY(reader,generation,course));
                PRAGMA user_version=29;
                """)
            }
            if schema < 30 {
                try execute("""
                CREATE TABLE course_switch_confirmations(job TEXT PRIMARY KEY NOT NULL REFERENCES jobs(id),
                    payload BLOB NOT NULL CHECK(length(payload)=132));
                PRAGMA user_version=30;
                """)
            }
            if schema < 31 {
                try execute("""
                CREATE TABLE firmware_installations(job TEXT PRIMARY KEY NOT NULL REFERENCES jobs(id),
                    payload BLOB NOT NULL CHECK(length(payload)=104),
                    verified INTEGER NOT NULL CHECK(verified IN (0,1)));
                PRAGMA user_version=31;
                """)
            }
            if schema < 32 {
                try execute("""
                CREATE TABLE job_font_destinations(job TEXT PRIMARY KEY NOT NULL REFERENCES jobs(id),
                    destination TEXT NOT NULL CHECK(length(CAST(destination AS BLOB)) BETWEEN 1 AND 127));
                PRAGMA user_version=32;
                """)
            }
            if schema < 33 {
                try execute("""
                CREATE TABLE removal_jobs(id TEXT PRIMARY KEY NOT NULL,
                    reader BLOB NOT NULL CHECK(length(reader)=16),
                    request BLOB NOT NULL CHECK(length(request)=115),
                    phase TEXT NOT NULL CHECK(phase IN ('queued','removing','paused','completed')));
                PRAGMA user_version=33;
                """)
            }
            if schema < 34 {
                try execute("""
                CREATE TABLE tinta_migration_jobs(id BLOB PRIMARY KEY NOT NULL CHECK(length(id)=16),
                    backup TEXT NOT NULL REFERENCES legacy_backups(hash),reader BLOB NOT NULL CHECK(length(reader)=16),
                    generation BLOB NOT NULL CHECK(length(generation)=16),admission BLOB NOT NULL CHECK(length(admission)=256),
                    phase TEXT NOT NULL CHECK(phase IN ('queued','admitting','transferring','committing','completed')),
                    acknowledged INTEGER NOT NULL CHECK(acknowledged>=0),paused INTEGER NOT NULL CHECK(paused IN (0,1)));
                CREATE TABLE tinta_migration_events(job BLOB NOT NULL REFERENCES tinta_migration_jobs(id),
                    ordinal INTEGER NOT NULL CHECK(ordinal>=0),payload BLOB NOT NULL CHECK(length(payload)<=1024),
                    PRIMARY KEY(job,ordinal));
                CREATE UNIQUE INDEX tinta_migration_active_reader ON tinta_migration_jobs(reader,generation) WHERE phase!='completed';
                PRAGMA user_version=34;
                """)
            }
            if schema < 35 {
                try execute("""
                ALTER TABLE tinta_migration_jobs ADD COLUMN abort_state INTEGER NOT NULL DEFAULT 0 CHECK(abort_state IN (0,1,2));
                DROP INDEX tinta_migration_active_reader;
                CREATE UNIQUE INDEX tinta_migration_active_reader ON tinta_migration_jobs(reader,generation)
                    WHERE phase!='completed' AND abort_state!=2;
                PRAGMA user_version=35;
                """)
            }
            if schema < 36 {
                try execute("""
                CREATE TABLE legacy_preference_imports(backup TEXT PRIMARY KEY NOT NULL REFERENCES legacy_backups(hash),
                    preferences BLOB NOT NULL CHECK(length(preferences)=72),identities BLOB NOT NULL CHECK(length(identities)=288));
                PRAGMA user_version=36;
                """)
            }
            if schema < 37 {
                try execute("""
                CREATE TABLE journal_merge_jobs(id BLOB PRIMARY KEY NOT NULL CHECK(length(id)=16),
                    reader BLOB NOT NULL CHECK(length(reader)=16),generation BLOB NOT NULL CHECK(length(generation)=16),
                    declaration BLOB NOT NULL CHECK(length(declaration)=136),
                    phase TEXT NOT NULL CHECK(phase IN ('queued','transferring','committing','completed')),
                    acknowledged INTEGER NOT NULL CHECK(acknowledged>=0),paused INTEGER NOT NULL CHECK(paused IN (0,1)));
                CREATE TABLE journal_merge_events(job BLOB NOT NULL REFERENCES journal_merge_jobs(id),
                    ordinal INTEGER NOT NULL CHECK(ordinal>=0),payload BLOB NOT NULL CHECK(length(payload)<=1024),
                    PRIMARY KEY(job,ordinal));
                CREATE UNIQUE INDEX journal_merge_active_reader ON journal_merge_jobs(reader,generation) WHERE phase!='completed';
                PRAGMA user_version=37;
                """)
            }
            if schema < 38 {
                try execute("""
                ALTER TABLE journal_merge_jobs ADD COLUMN abort_state INTEGER NOT NULL DEFAULT 0 CHECK(abort_state IN (0,1,2));
                DROP INDEX journal_merge_active_reader;
                CREATE UNIQUE INDEX journal_merge_active_reader ON journal_merge_jobs(reader,generation)
                    WHERE phase!='completed' AND abort_state!=2;
                PRAGMA user_version=38;
                """)
            }
            if schema < 39 {
                try execute("""
                CREATE TABLE reader_import_jobs(id TEXT PRIMARY KEY NOT NULL,
                    reader BLOB NOT NULL CHECK(length(reader)=16),generation BLOB NOT NULL CHECK(length(generation)=16),
                    installation BLOB NOT NULL CHECK(length(installation)=16),manifest BLOB NOT NULL CHECK(length(manifest)=63),
                    content TEXT NOT NULL CHECK(length(content)=64),acknowledged INTEGER NOT NULL CHECK(acknowledged>=0),
                    phase TEXT NOT NULL CHECK(phase IN ('queued','downloading','paused','verifying','completed','aborted')));
                CREATE UNIQUE INDEX reader_import_active_content ON reader_import_jobs(reader,generation,content)
                    WHERE phase NOT IN ('completed','aborted');
                PRAGMA user_version=39;
                """)
            }
            if schema < 40 {
                try execute("""
                CREATE TABLE reader_import_filenames(id TEXT PRIMARY KEY NOT NULL
                    REFERENCES reader_import_jobs(id),original_filename TEXT NOT NULL
                    CHECK(length(CAST(original_filename AS BLOB)) BETWEEN 1 AND 255));
                PRAGMA user_version=40;
                """)
            }
            if schema < 41 {
                try execute("""
                CREATE TABLE course_baseline_confirmations(job TEXT PRIMARY KEY NOT NULL REFERENCES jobs(id),
                    payload BLOB NOT NULL CHECK(length(payload)=155),
                    review BLOB NOT NULL CHECK(length(review) BETWEEN 608 AND 4416));
                PRAGMA user_version=41;
                """)
            }
            try execute("COMMIT")
            committed = true
        } catch { sqlite3_close_v2(handle); handle = nil; throw error }
    }
    deinit { sqlite3_close_v2(handle) }
    func query(_ sql: String, _ values: [Binding] = []) throws -> Statement {
        guard let handle else { throw StoreError.invalidValue }
        return try Statement(handle: handle, sql: sql, values: values)
    }
    var changedRows: Int32 { sqlite3_changes(handle) }
    func execute(_ sql: String, _ values: [Binding] = []) throws {
        if values.isEmpty {
            guard sqlite3_exec(handle, sql, nil, nil, nil) == SQLITE_OK else { throw StoreError.database(String(cString: sqlite3_errmsg(handle))) }
        } else {
            let statement = try query(sql, values)
            while try statement.next() {}
        }
    }
}
private final class Statement {
    private var handle: OpaquePointer?
    private let database: OpaquePointer
    init(handle database: OpaquePointer, sql: String, values: [Binding]) throws {
        var prepared: OpaquePointer?
        guard sqlite3_prepare_v2(database, sql, -1, &prepared, nil) == SQLITE_OK, let prepared else {
            if let prepared { sqlite3_finalize(prepared) }
            throw StoreError.database(String(cString: sqlite3_errmsg(database)))
        }
        handle = prepared; self.database = database
        let transient = unsafeBitCast(-1, to: sqlite3_destructor_type.self)
        for (offset, value) in values.enumerated() {
            let index = Int32(offset + 1)
            let status: Int32
            switch value {
            case .text(let text):
                guard text.utf8.count <= Int(Int32.max) else { sqlite3_finalize(handle); handle = nil; throw StoreError.invalidValue }
                status = text.withCString { sqlite3_bind_text(handle, index, $0, Int32(text.utf8.count), transient) }
            case .blob(let bytes):
                guard bytes.count <= Int(Int32.max) else { sqlite3_finalize(handle); handle = nil; throw StoreError.invalidValue }
                status = bytes.isEmpty ? sqlite3_bind_zeroblob(handle, index, 0) : bytes.withUnsafeBytes { sqlite3_bind_blob(handle, index, $0.baseAddress, Int32($0.count), transient) }
            case .integer(let integer): status = sqlite3_bind_int64(handle, index, integer)
            }
            guard status == SQLITE_OK else { sqlite3_finalize(handle); handle = nil; throw StoreError.database(String(cString: sqlite3_errmsg(database))) }
        }
    }
    deinit { sqlite3_finalize(handle) }
    func next() throws -> Bool {
        let result = sqlite3_step(handle)
        if result == SQLITE_ROW { return true }
        guard result == SQLITE_DONE else { throw StoreError.database(String(cString: sqlite3_errmsg(database))) }
        return false
    }
    func integer(_ column: Int32) -> Int64 { sqlite3_column_int64(handle, column) }
    func text(_ column: Int32) -> String {
        guard let bytes = sqlite3_column_text(handle, column) else { return "" }
        return String(decoding: UnsafeBufferPointer(start: bytes, count: Int(sqlite3_column_bytes(handle, column))), as: UTF8.self)
    }
    func blob(_ column: Int32) -> Data {
        guard let bytes = sqlite3_column_blob(handle, column) else { return Data() }
        return Data(bytes: bytes, count: Int(sqlite3_column_bytes(handle, column)))
    }
}

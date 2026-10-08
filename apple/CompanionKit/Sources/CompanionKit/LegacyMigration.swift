import Foundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif

public enum LegacyMigrationError: Error, Equatable, Sendable {
    case unrepresentableFlags(Int), stateMismatch, invalidExpectedState, invalidUndo(Int)
    case overlapConfirmationRequired([LegacyHistoryOverlap]), invalidOverlapDecision, staleBackupSet
}
public struct LegacyMigrationContext: Sendable {
    public let origin: Data
    public let epoch: UInt64
    public let generation: Data
    public let resource: Data
    public let configuration: SchedulerConfiguration
    public init(origin: Data, epoch: UInt64, generation: Data, resource: Data, configuration: SchedulerConfiguration) throws {
        _ = try EventIdentity(origin: origin, epoch: epoch, sequence: 1)
        guard generation.count == 16, generation.contains(where: { $0 != 0 }), resource.count == 32,
              resource.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        self.origin = origin; self.epoch = epoch; self.generation = generation; self.resource = resource; self.configuration = configuration
    }
}
public struct LegacyMigrationPlan: Sendable {
    public let originalHash: Data
    public let mutations: [JournalMutation]
    public let snapshot: TintaSnapshot
}
public extension LegacyTintaJournal {
    // Context must use a durably reserved migration epoch and a confirmed rebuild configuration.
    func migrationPlan(context: LegacyMigrationContext, expectedItems: [UInt32: ScheduledItem]) throws -> LegacyMigrationPlan {
        let mutationCount = entries.reduce(0) { count, entry in
            if case .setFlags = entry.operation { return count + 2 }
            return count + 1
        }
        var mutations: [JournalMutation] = []; mutations.reserveCapacity(mutationCount)
        var states: [UInt32: ScheduledItem] = [:]; states.reserveCapacity(expectedItems.count)
        var lastReview: (record: Int, identity: EventIdentity, before: ScheduledItem)?
        let configurationHash = Data(SHA256.hash(data: context.configuration.encoded))
        func append(_ value: TintaValue, subject: TintaSubject, entry: LegacyEntry) throws -> EventIdentity {
            let identity = try EventIdentity(origin: context.origin, epoch: context.epoch, sequence: UInt64(mutations.count) + 1)
            let body = try TintaBody(subject: subject, value: value)
            var ancestors: [EventIdentity] = []; ancestors.reserveCapacity(2)
            if let previous = mutations.last { ancestors.append(previous.event.identity) }
            if case let .undo(target) = value, !ancestors.contains(target) { ancestors.append(target) }
            let review: Bool
            if case .review = value { review = true } else { review = false }
            let event = try SyncEvent(identity: identity, storageGeneration: context.generation, kind: value.kind,
                                      resource: context.resource, bodyHash: Data(SHA256.hash(data: body.encoded)),
                                      studyDay: UInt32(entry.studyDay), schedulerVersion: review ? 1 : 0,
                                      schedulerConfiguration: review ? configurationHash : Data(count: 32), ancestors: ancestors)
            let mutation = try JournalMutation(event: event, body: body.encoded)
            _ = try TintaBody(mutation: mutation)
            mutations.append(mutation)
            return identity
        }
        for (index, entry) in entries.enumerated() {
            let subject = try TintaSubject(course: courseIdentity, uid: entry.uid)
            let before = try states[entry.uid] ?? ScheduledItem(uid: entry.uid)
            switch entry.operation {
            case let .review(grade, format, response):
                let identity = try append(.review(grade: grade, format: format, responseMilliseconds: UInt32(response) * 250,
                                                  configuration: context.configuration), subject: subject, entry: entry)
                states[entry.uid] = try before.reviewed(grade: grade, day: entry.studyDay, configuration: context.configuration)
                lastReview = (index, identity, before)
            case let .undo(record):
                guard let target = lastReview, target.record == record else { throw LegacyMigrationError.invalidUndo(index) }
                _ = try append(.undo(target.identity), subject: subject, entry: entry)
                states[entry.uid] = target.before; lastReview = nil
            case let .setFlags(flags):
                guard before.bytes[14] & 2 == flags & 2 else { throw LegacyMigrationError.unrepresentableFlags(index) }
                _ = try append(.suspension(flags & 1 != 0), subject: subject, entry: entry)
                _ = try append(.star(flags & 4 != 0), subject: subject, entry: entry)
                var bytes = before.bytes; bytes[14] = flags
                states[entry.uid] = try ScheduledItem(decoding: bytes); lastReview = nil
            }
        }
        let snapshot = try TintaHistory.replay(mutations)
        var expected: [TintaSubject: ScheduledItem] = [:]; expected.reserveCapacity(expectedItems.count)
        for (uid, item) in expectedItems {
            var reader = ByteReader(item.bytes)
            guard try reader.number(4) == UInt64(uid) else { throw LegacyMigrationError.invalidExpectedState }
            expected[try TintaSubject(course: courseIdentity, uid: uid)] = item
        }
        guard snapshot.items == expected else { throw LegacyMigrationError.stateMismatch }
        return LegacyMigrationPlan(originalHash: originalHash, mutations: mutations, snapshot: snapshot)
    }
}

public struct LegacyBackupSnapshot: Sendable {
    public let manifest: LegacyBackupManifest
    public let journal: LegacyTintaJournal
    public let items: LegacyItemStore
    public let profile: LegacyProfile
    public let readings: LegacyMarkLog?
    public let starred: LegacyMarkLog?
}
public extension ContentVault {
    func legacyBackupSnapshot(_ backup: ContentID) throws -> LegacyBackupSnapshot {
        let manifest = try verifiedLegacyBackup(backup)
        func load(_ role: LegacyBackupRole, limit: UInt64) throws -> Data? {
            guard let file = manifest.files.first(where: { $0.role == role }) else { return nil }
            guard file.length <= limit else { throw LegacyJournalError.tooLarge }
            let object = try verifiedObject(file.id)
            return try Data(contentsOf: object.url)
        }
        let limit = UInt64(LegacyTintaJournal.maximumByteCount)
        guard let reviews = try load(.reviews, limit: limit), let items = try load(.items, limit: limit),
              let profile = try load(.profile, limit: 65547) else { throw StoreError.invalidValue }
        let journal = try LegacyTintaJournal(courseIdentity: manifest.course, bytes: reviews)
        let state = try LegacyItemStore(bytes: items)
        guard UInt64(state.journalCount) == UInt64(journal.entries.count) else { throw LegacyMigrationError.stateMismatch }
        let readings = try load(.readings, limit: limit).map { try LegacyMarkLog(bytes: $0) }
        let starred = try load(.starred, limit: limit).map { try LegacyMarkLog(bytes: $0) }
        return LegacyBackupSnapshot(manifest: manifest, journal: journal, items: state,
            profile: try LegacyProfile(bytes: profile), readings: readings, starred: starred)
    }
    func legacyMigrationPlan(backup: ContentID, context: LegacyMigrationContext) throws -> LegacyMigrationPlan {
        let snapshot = try legacyBackupSnapshot(backup)
        guard context.generation == snapshot.manifest.generation else { throw StoreError.invalidValue }
        return try snapshot.journal.migrationPlan(context: context, expectedItems: snapshot.items.items)
    }
}

public extension ContentVault {
    func legacyReadingOptions(backup: ContentID, course: ContentID,
                             confirmedCourseIdentity: Data) throws -> [LegacyReadingConflictDetails] {
        let source = try legacyBackupSnapshot(backup)
        guard source.manifest.course == confirmedCourseIdentity else { throw StoreError.invalidValue }
        let object = try verifiedObject(course)
        let metadata = try CoursePackInspector.inspect(object.url)
        var stories: [UInt32: CourseStoryContext] = [:]; stories.reserveCapacity(metadata.stories.count)
        for story in metadata.stories {
            guard stories.updateValue(story, forKey: story.id) == nil else { throw StoreError.invalidValue }
        }
        return try (source.readings?.readingConflicts(course: metadata) ?? []).map { conflict in
            let candidates = try conflict.candidates.map { id in
                guard let story = stories[id] else { throw StoreError.invalidValue }
                return story
            }
            return LegacyReadingConflictDetails(legacyKey: conflict.legacyKey, candidates: candidates)
        }
    }
    func legacyReadingConflicts(backup: ContentID, course: ContentID,
                               confirmedCourseIdentity: Data) throws -> [LegacyReadingConflict] {
        let source = try legacyBackupSnapshot(backup)
        guard source.manifest.course == confirmedCourseIdentity else { throw StoreError.invalidValue }
        let object = try verifiedObject(course)
        let metadata = try CoursePackInspector.inspect(object.url)
        return try source.readings?.readingConflicts(course: metadata) ?? []
    }
    // Legacy packs have no logical course identity; association must be confirmed before calling.
    func legacyMigrationPlan(backup: ContentID, context: LegacyMigrationContext, course: ContentID,
                             confirmedCourseIdentity: Data,
                             confirmedReadingResolutions: [UInt32: Set<UInt32>] = [:]) throws -> LegacyMigrationPlan {
        let source = try legacyBackupSnapshot(backup)
        guard confirmedCourseIdentity == source.manifest.course, context.generation == source.manifest.generation,
              context.resource.map({ String(format: "%02x", $0) }).joined() == course.hex else { throw StoreError.invalidValue }
        let object = try verifiedObject(course)
        let metadata = try CoursePackInspector.inspect(object.url)
        let lessons = try source.profile.completedLessonIdentities(course: metadata)
        guard source.readings != nil || confirmedReadingResolutions.isEmpty else { throw StoreError.invalidValue }
        let readings = try source.readings?.completedStoryIdentities(course: metadata,
            confirmedResolutions: confirmedReadingResolutions) ?? []
        let knownItems = Set(metadata.itemIdentities), recognition = Set(metadata.recognitionItems)
        for uid in source.items.items.keys where !knownItems.contains(uid) { throw LegacyMarkLogError.unknownKey(uid) }
        let stars = source.starred?.keys ?? []
        for uid in stars {
            guard recognition.contains(uid) else { throw LegacyMarkLogError.unknownKey(uid) }
            if let item = source.items.items[uid], item.bytes[11] & 3 != 0 { throw LegacyMigrationError.stateMismatch }
        }
        let reviews = try source.journal.migrationPlan(context: context, expectedItems: source.items.items)
        var mutations = reviews.mutations
        mutations.reserveCapacity(mutations.count + lessons.count + readings.count + stars.count)
        func append(_ uid: UInt32, value: TintaValue) throws {
            let body = try TintaBody(subject: TintaSubject(course: source.manifest.course, uid: uid), value: value)
            let identity = try EventIdentity(origin: context.origin, epoch: context.epoch, sequence: UInt64(mutations.count) + 1)
            let event = try SyncEvent(identity: identity, storageGeneration: context.generation, kind: value.kind,
                resource: context.resource, bodyHash: Data(SHA256.hash(data: body.encoded)),
                ancestors: mutations.last.map { [$0.event.identity] } ?? [])
            mutations.append(try JournalMutation(event: event, body: body.encoded))
        }
        for uid in lessons { try append(uid, value: .lessonComplete(true)) }
        for uid in readings { try append(uid, value: .readingComplete(true)) }
        for uid in stars { try append(uid, value: .star(true)) }
        return LegacyMigrationPlan(originalHash: reviews.originalHash, mutations: mutations,
                                   snapshot: try TintaHistory.replay(mutations))
    }
}

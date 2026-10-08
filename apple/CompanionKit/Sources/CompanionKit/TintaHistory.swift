import Foundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif

public struct TintaSubject: Hashable, Sendable {
    public let course: Data
    public let uid: UInt32
    public init(course: Data, uid: UInt32) throws {
        guard course.count == 16, course.contains(where: { $0 != 0 }), uid > 0, uid < UInt32.max else { throw ProtocolError.value }
        self.course = course; self.uid = uid
    }
}
public enum TintaValue: Equatable, Sendable {
    case review(grade: UInt8, format: UInt8, responseMilliseconds: UInt32, configuration: SchedulerConfiguration)
    case undo(EventIdentity)
    case suspension(Bool), star(Bool), lessonComplete(Bool), readingComplete(Bool)
    var kind: SyncEventKind {
        switch self {
        case .review: .review
        case .undo: .undoReview
        case .suspension: .suspension
        case .star: .star
        case .lessonComplete: .lessonComplete
        case .readingComplete: .readingComplete
        }
    }
}
public struct TintaBody: Equatable, Sendable {
    public let subject: TintaSubject
    public let value: TintaValue
    public init(subject: TintaSubject, value: TintaValue) throws {
        if case let .review(grade, format, _, _) = value {
            guard (1 ... 4).contains(grade), format <= 9 else { throw ProtocolError.value }
        }
        self.subject = subject; self.value = value
    }
    public var encoded: Data {
        var bytes = Data([1, value.kind.rawValue]); bytes.reserveCapacity(54)
        bytes.append(subject.course); bytes.appendLittleEndian(UInt64(subject.uid), count: 4)
        switch value {
        case let .review(grade, format, response, configuration):
            bytes.append(grade); bytes.append(format); bytes.appendLittleEndian(UInt64(response), count: 4)
            bytes.append(configuration.encoded)
        case let .undo(identity): bytes.append(identity.storageKey)
        case let .suspension(value), let .star(value), let .lessonComplete(value), let .readingComplete(value): bytes.append(value ? 1 : 0)
        }
        return bytes
    }
    public init(decoding bytes: Data) throws {
        var reader = ByteReader(bytes)
        guard try reader.number(1) == 1 else { throw ProtocolError.version }
        guard let kind = SyncEventKind(rawValue: UInt8(try reader.number(1))) else { throw ProtocolError.value }
        let subject = try TintaSubject(course: reader.take(16), uid: UInt32(reader.number(4)))
        let value: TintaValue
        switch kind {
        case .review:
            let grade = UInt8(try reader.number(1)), format = UInt8(try reader.number(1)), response = UInt32(try reader.number(4))
            guard try reader.number(1) == 1, try reader.number(1) == 1 else { throw ProtocolError.version }
            let configuration = try SchedulerConfiguration(retentionBasisPoints: UInt16(reader.number(2)), maximumInterval: UInt16(reader.number(2)))
            value = .review(grade: grade, format: format, responseMilliseconds: response, configuration: configuration)
        case .undoReview: value = .undo(try EventIdentity.decodeBody(&reader))
        case .suspension, .star, .lessonComplete, .readingComplete:
            let flag = try reader.number(1); guard flag <= 1 else { throw ProtocolError.value }
            switch kind {
            case .suspension: value = .suspension(flag == 1)
            case .star: value = .star(flag == 1)
            case .lessonComplete: value = .lessonComplete(flag == 1)
            default: value = .readingComplete(flag == 1)
            }
        default: throw ProtocolError.value
        }
        guard reader.position == bytes.count else { throw ProtocolError.length }
        try self.init(subject: subject, value: value)
    }
    public init(mutation: JournalMutation) throws {
        try self.init(decoding: mutation.body)
        let event = mutation.event
        guard value.kind == event.kind, event.studyDay <= UInt16.max,
              event.resource.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        if case let .review(_, _, _, configuration) = value {
            guard event.schedulerVersion == 1, event.schedulerConfiguration == Data(SHA256.hash(data: configuration.encoded)) else { throw ProtocolError.value }
        } else {
            guard event.schedulerVersion == 0, event.schedulerConfiguration == Data(count: 32) else { throw ProtocolError.value }
        }
        if case let .undo(target) = value {
            guard event.ancestors.contains(target) else { throw ProtocolError.value }
        }
    }
}
private extension EventIdentity {
    static func decodeBody(_ reader: inout ByteReader) throws -> Self {
        try Self(origin: reader.take(16), epoch: reader.number(8), sequence: reader.number(8))
    }
}

public struct StudyTotals: Equatable, Sendable {
    public var newItems: UInt32 = 0
    public var reviews: UInt32 = 0
    public var gradedReviews: UInt32 = 0
    public var correctReviews: UInt32 = 0
    public var responseMilliseconds: UInt64 = 0

    public func readerDayTotals() throws -> TintaDayTotals {
        let seconds = responseMilliseconds / 1000 + (responseMilliseconds % 1000 >= 500 ? 1 : 0)
        guard correctReviews <= gradedReviews, newItems <= gradedReviews,
              let representedSeconds = UInt32(exactly: seconds) else { throw ProtocolError.value }
        return TintaDayTotals(reviews: gradedReviews, correct: correctReviews,
                             newItems: newItems, seconds: representedSeconds)
    }
}
public struct TintaDayTotals: Equatable, Sendable {
    public let reviews: UInt32
    public let correct: UInt32
    public let newItems: UInt32
    public let seconds: UInt32
}
public struct TintaSnapshot: Equatable, Sendable {
    public let items: [TintaSubject: ScheduledItem]
    public let completedLessons: Set<TintaSubject>
    public let completedReadings: Set<TintaSubject>
    public let studyTotals: [Data: [UInt16: StudyTotals]]
    public let undoneReviews: Set<EventIdentity>
}
public enum TintaReplayError: Error, Equatable, Sendable { case invalidUndo(EventIdentity) }

public enum TintaHistory {
    public static func replay(_ deliveries: [JournalMutation], course: Data? = nil) throws -> TintaSnapshot {
        if let course {
            guard course.count == 16, course.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        }
        let ordered = try SyncHistory.merged(deliveries.map(\.event))
        var bodies: [EventIdentity: TintaBody] = [:]; bodies.reserveCapacity(ordered.count)
        for mutation in deliveries where mutation.event.kind.rawValue >= SyncEventKind.review.rawValue {
            bodies[mutation.event.identity] = try TintaBody(mutation: mutation)
        }
        var undone = Set<EventIdentity>(); undone.reserveCapacity(ordered.count)
        for event in ordered {
            guard let body = bodies[event.identity] else { continue }
            if case let .undo(target) = body.value {
                guard let reviewed = bodies[target], reviewed.subject == body.subject,
                      case .review = reviewed.value else { throw TintaReplayError.invalidUndo(target) }
                undone.insert(target)
            }
        }
        var items: [TintaSubject: ScheduledItem] = [:]; items.reserveCapacity(ordered.count)
        var lessons = Set<TintaSubject>(), readings = Set<TintaSubject>()
        lessons.reserveCapacity(32); readings.reserveCapacity(32)
        var totals: [Data: [UInt16: StudyTotals]] = [:]; totals.reserveCapacity(8)
        for event in ordered {
            guard let body = bodies[event.identity] else { continue }
            let subject = body.subject
            if let course, subject.course != course { continue }
            switch body.value {
            case let .review(grade, _, milliseconds, configuration):
                if undone.contains(event.identity) {
                    if items[subject] == nil { items[subject] = try ScheduledItem(uid: subject.uid) }
                    continue
                }
                let before = try items[subject] ?? ScheduledItem(uid: subject.uid)
                let day = UInt16(event.studyDay)
                var count = totals[subject.course]?[day] ?? StudyTotals()
                let applied = try before.reviewedWithCounts(grade: grade, day: day, configuration: configuration)
                if applied.newItem { count.newItems += 1 }
                if applied.review { count.reviews += 1 }
                count.gradedReviews += 1
                if applied.correct { count.correctReviews += 1 }
                count.responseMilliseconds += UInt64(milliseconds)
                totals[subject.course, default: [:]][day] = count
                items[subject] = applied.item
            case let .suspension(enabled), let .star(enabled):
                let before = try items[subject] ?? ScheduledItem(uid: subject.uid)
                items[subject] = try before.settingFlag(event.kind == .suspension ? .suspension : .star, enabled: enabled)
            case let .lessonComplete(enabled):
                if enabled { lessons.insert(subject) } else { lessons.remove(subject) }
            case let .readingComplete(enabled):
                if enabled { readings.insert(subject) } else { readings.remove(subject) }
            case .undo: break
            }
        }
        let selectedUndo = course.map { course in undone.filter { bodies[$0]?.subject.course == course } } ?? undone
        return TintaSnapshot(items: items, completedLessons: lessons, completedReadings: readings,
                             studyTotals: totals, undoneReviews: selectedUndo)
    }
}

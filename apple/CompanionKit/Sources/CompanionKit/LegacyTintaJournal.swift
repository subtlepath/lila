import Foundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif

public enum LegacyJournalError: Error, Equatable, Sendable {
    case tooLarge, invalidCourse, truncated(Int), invalidRecord(Int), invalidUndo(Int), nonzeroTail(Int)
}
public enum LegacyOperation: Equatable, Sendable {
    case review(grade: UInt8, format: UInt8, responseQuarterSeconds: UInt8)
    case undo(reviewRecord: Int)
    case setFlags(UInt8)
}
public struct LegacyEntry: Equatable, Sendable {
    public let uid: UInt32
    public let timestamp: UInt32
    public let studyDay: UInt16
    public let operation: LegacyOperation
    public let bytes: Data
}
public enum LegacyOverlap: Equatable, Sendable {
    case differentCourses, noSharedPrefix
    // Identical records do not establish that two devices share the same actual reviews.
    case needsConfirmation(prefixRecords: Int, identicalHistory: Bool)
}
public struct LegacyTintaJournal: Sendable {
    public static let maximumByteCount = 16 * 1024 * 1024
    public let courseIdentity: Data
    public let originalBytes: Data
    public let originalHash: Data
    public let entries: [LegacyEntry]
    public let zeroTailBytes: Int
    public let effectiveReviewCount: Int
    public init(courseIdentity: Data, bytes: Data) throws {
        guard courseIdentity.count == 16, courseIdentity.contains(where: { $0 != 0 }) else { throw LegacyJournalError.invalidCourse }
        guard bytes.count <= Self.maximumByteCount else { throw LegacyJournalError.tooLarge }
        var reader = ByteReader(bytes)
        var entries: [LegacyEntry] = []; entries.reserveCapacity(min(bytes.count / 12, 65536))
        var lastReview: (uid: UInt32, record: Int)?
        var reviews = 0, zeroTail = 0
        while reader.position < bytes.count {
            let offset = reader.position, index = entries.count
            let remaining = bytes.count - offset
            if remaining < 12 {
                guard try reader.take(remaining).allSatisfy({ $0 == 0 }) else { throw LegacyJournalError.truncated(index) }
                zeroTail = remaining; break
            }
            let raw = try reader.take(12)
            if raw.allSatisfy({ $0 == 0 }) {
                guard try reader.take(bytes.count - reader.position).allSatisfy({ $0 == 0 }) else { throw LegacyJournalError.nonzeroTail(index) }
                zeroTail = bytes.count - offset; break
            }
            var record = ByteReader(raw)
            let uid = UInt32(try record.number(4)), timestamp = UInt32(try record.number(4)), day = UInt16(try record.number(2))
            let op = UInt8(try record.number(1)), argument = UInt8(try record.number(1))
            guard uid > 0, uid < UInt32.max else { throw LegacyJournalError.invalidRecord(index) }
            let grade = op & 7, code = op >> 3
            let operation: LegacyOperation
            if (1 ... 4).contains(grade) {
                guard code <= 9 else { throw LegacyJournalError.invalidRecord(index) }
                operation = .review(grade: grade, format: code, responseQuarterSeconds: argument)
                lastReview = (uid, index); reviews += 1
            } else if grade == 0, code == 1 {
                guard argument == 0, let target = lastReview, target.uid == uid else { throw LegacyJournalError.invalidUndo(index) }
                operation = .undo(reviewRecord: target.record)
                lastReview = nil
                reviews -= 1
            } else if grade == 0, code == 2 {
                guard argument & ~7 == 0 else { throw LegacyJournalError.invalidRecord(index) }
                operation = .setFlags(argument); lastReview = nil
            } else { throw LegacyJournalError.invalidRecord(index) }
            entries.append(LegacyEntry(uid: uid, timestamp: timestamp, studyDay: day, operation: operation, bytes: raw))
        }
        self.courseIdentity = courseIdentity; originalBytes = Data(bytes)
        originalHash = Data(SHA256.hash(data: bytes)); self.entries = entries
        zeroTailBytes = zeroTail; effectiveReviewCount = reviews
    }
    public func overlap(with other: Self) -> LegacyOverlap {
        guard courseIdentity == other.courseIdentity else { return .differentCourses }
        var prefix = 0
        while prefix < min(entries.count, other.entries.count), entries[prefix].bytes == other.entries[prefix].bytes { prefix += 1 }
        guard prefix > 0 else { return .noSharedPrefix }
        return .needsConfirmation(prefixRecords: prefix, identicalHistory: prefix == entries.count && prefix == other.entries.count)
    }
}

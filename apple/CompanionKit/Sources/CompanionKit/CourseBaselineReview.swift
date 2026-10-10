import Foundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif

public enum CourseBaselineReviewDomain: UInt8, Sendable { case learner = 1, journal = 2, isolation = 3 }
public struct CourseBaselineReviewFile: Equatable, Sendable {
    public let domain: CourseBaselineReviewDomain
    public let present: Bool
    public let name: String
    public let length: UInt64
    public let hash: Data
}

/// Validates the frozen file roster; file hashes still require reader-side evidence verification.
public struct CourseBaselineReview: Equatable, Sendable {
    public static let maximumSize = 64 + 64 * 68
    public let reader: Data
    public let generation: Data
    public let course: Data
    public let journalPresent: Bool
    public let isolated: Bool
    public let files: [CourseBaselineReviewFile]
    public let encoded: Data
    public var hash: Data { Data(SHA256.hash(data: encoded)) }

    public init(decoding bytes: Data) throws {
        guard bytes.count >= 64, bytes.count <= Self.maximumSize else { throw ProtocolError.length }
        var cursor = ByteReader(bytes)
        guard try cursor.take(4) == Data([0x54, 0x43, 0x42, 0x56]) else { throw ProtocolError.version }
        let version = try cursor.number(1)
        let journal = try cursor.number(1)
        let scope = try cursor.number(1)
        guard (version == 1 && scope == 0) || (version == 2 && scope == 1) else { throw ProtocolError.version }
        guard journal <= 1, try cursor.number(1) == 0 else { throw ProtocolError.value }
        let reader = try cursor.take(16), generation = try cursor.take(16), course = try cursor.take(16)
        guard [reader, generation, course].allSatisfy({ $0.contains(where: { $0 != 0 }) }) else {
            throw ProtocolError.value
        }
        let count = Int(try cursor.number(2))
        guard count >= 8, count <= 64, bytes.count == 64 + count * 68 else { throw ProtocolError.length }
        guard try cursor.number(2) == 0 else { throw ProtocolError.value }
        var files: [CourseBaselineReviewFile] = []
        files.reserveCapacity(count)
        let journalNames = ["events.bin", "header-a.bin", "header-b.bin"]
        let isolationNames = ["mark-done", "mark-intent", "state-done", "state-intent"]
        var learners = 0, journals = 0, isolation = 0, presentJournal = 0
        for _ in 0..<count {
            guard let domain = CourseBaselineReviewDomain(rawValue: UInt8(try cursor.number(1))) else {
                throw ProtocolError.value
            }
            let present = try cursor.number(1), nameSize = Int(try cursor.number(1))
            guard present <= 1, nameSize > 0, nameSize < 24, try cursor.number(1) == 0 else {
                throw ProtocolError.value
            }
            let paddedName = try cursor.take(24)
            let nameBytes = paddedName.prefix(nameSize)
            guard paddedName.dropFirst(nameSize).allSatisfy({ $0 == 0 }), nameBytes.allSatisfy({
                (97...122).contains($0) || (48...57).contains($0) || $0 == 46 || $0 == 45 || $0 == 95
            }), let name = String(data: Data(nameBytes), encoding: .utf8), name != ".", name != ".." else {
                throw ProtocolError.value
            }
            if let previous = files.last {
                guard domain.rawValue > previous.domain.rawValue ||
                      (domain == previous.domain && name > previous.name) else { throw ProtocolError.value }
            }
            let length = try cursor.number(8), hash = try cursor.take(32)
            let nonzeroHash = hash.contains(where: { $0 != 0 })
            guard length <= UInt64(UInt32.max),
                  present == 1 ? nonzeroHash : (length == 0 && !nonzeroHash) else { throw ProtocolError.value }
            switch domain {
            case .learner:
                guard present == 1, !name.hasPrefix("pack-"),
                      ![".tmp", ".sync", ".sync-old", ".proof"].contains(where: { name.hasSuffix($0) }),
                      !["sync-intent", "sync-intent-stage", "sync-receipt-stage"].contains(name) else {
                    throw ProtocolError.value
                }
                learners += 1
            case .journal:
                guard journals < journalNames.count, name == journalNames[journals] else { throw ProtocolError.value }
                if present == 1 { presentJournal |= 1 << journals }
                journals += 1
            case .isolation:
                guard isolation < isolationNames.count, name == isolationNames[isolation],
                      scope == 1 ? present == 0 : (present == 1 && length == (isolation % 2 == 0 ? 24 : 28)) else {
                    throw ProtocolError.value
                }
                isolation += 1
            }
            files.append(CourseBaselineReviewFile(domain: domain, present: present == 1,
                name: name, length: length, hash: hash))
        }
        guard learners > 0, journals == 3, isolation == 4,
              journal == 1 ? ((presentJournal & 1) != 0 && (presentJournal & 6) != 0) : presentJournal == 0,
              try cursor.number(4) == UInt64(legacyCRC32(Data(bytes.dropLast(4)))) else { throw ProtocolError.value }
        self.reader = reader; self.generation = generation; self.course = course
        journalPresent = journal == 1; self.files = files; encoded = bytes
        isolated = scope == 0
    }

    public func matches(_ request: CourseBaselineImportRequest, reader: Data) -> Bool {
        isolated && self.reader == reader && generation == request.generation && course == request.manifest.logicalIdentity &&
        hash == request.reviewHash
    }
}

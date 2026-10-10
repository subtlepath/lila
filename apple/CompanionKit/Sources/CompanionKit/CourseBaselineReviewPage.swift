import Foundation

public struct CourseBaselineReviewPage: Equatable, Sendable {
    public static let maximumBytes = 976
    public let total: Int
    public let offset: Int
    public let hash: Data
    public let bytes: Data

    public init(decoding encoded: Data) throws {
        guard encoded.count > 48, encoded.count <= 1024 else { throw ProtocolError.length }
        var cursor = ByteReader(encoded)
        guard try cursor.take(6) == Data([0x54, 0x43, 0x42, 0x50, 1, 0]) else { throw ProtocolError.version }
        let total = Int(try cursor.number(2)), offset = Int(try cursor.number(2))
        let count = Int(try cursor.number(2)), hash = try cursor.take(32)
        guard total >= 608, total <= CourseBaselineReview.maximumSize, (total - 64) % 68 == 0,
              offset < total, count > 0, count <= Self.maximumBytes, count <= total - offset,
              encoded.count == 48 + count, hash.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        let bytes = try cursor.take(count)
        guard try cursor.number(4) == UInt64(legacyCRC32(Data(encoded.prefix(encoded.count - 4)))) else {
            throw ProtocolError.value
        }
        self.total = total
        self.offset = offset
        self.hash = hash
        self.bytes = bytes
    }
}

/// Collects one frozen roster; completion validates its digest and reader context,
/// but still requires explicit approval and native file evidence verification.
public struct CourseBaselineReviewAssembly: Sendable {
    private let reader: Data
    private let generation: Data
    private let course: Data
    private var total: Int?
    private var hash: Data?
    private var buffer = Data()
    private var completed: CourseBaselineReview?
    public var offset: Int { buffer.count }

    public init(reader: Data, generation: Data, course: Data) throws {
        guard [reader, generation, course].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }) else {
            throw ProtocolError.value
        }
        self.reader = reader
        self.generation = generation
        self.course = course
    }

    /// Rejects skipped/overlapping/changed pages without changing accepted state.
    /// An identical complete duplicate is harmless, including after completion.
    public mutating func append(_ encoded: Data) throws -> CourseBaselineReview? {
        let page = try CourseBaselineReviewPage(decoding: encoded)
        if let total, let hash {
            guard page.total == total, page.hash == hash else { throw ProtocolError.value }
        }
        if page.offset < buffer.count {
            guard page.bytes.count <= buffer.count - page.offset,
                  buffer.subdata(in: page.offset ..< page.offset + page.bytes.count) == page.bytes else {
                throw ProtocolError.value
            }
            return completed
        }
        guard page.offset == buffer.count, completed == nil else { throw ProtocolError.value }
        var candidate = buffer
        if total == nil { candidate.reserveCapacity(page.total) }
        candidate.append(page.bytes)
        var result: CourseBaselineReview?
        if candidate.count == page.total {
            let review = try CourseBaselineReview(decoding: candidate)
            guard review.hash == page.hash, review.reader == reader, review.generation == generation,
                  review.course == course else { throw ProtocolError.value }
            result = review
        }
        buffer = candidate
        total = page.total
        hash = page.hash
        completed = result
        return result
    }
}

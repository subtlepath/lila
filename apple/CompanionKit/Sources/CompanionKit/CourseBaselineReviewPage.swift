import Foundation

public enum CourseBaselineReviewControlError: UInt8, Error, Equatable, Sendable {
    case invalidRequest = 1, unauthorized, busy, wrongStorage, unavailable, unsupported
}

public struct CourseBaselineReviewPageRequest: Equatable, Sendable {
    public let generation: Data
    public let course: Data
    public let hash: Data
    public let offset: Int
    public let limit: Int

    public init(generation: Data, course: Data, hash: Data = Data(repeating: 0, count: 32),
                offset: Int = 0, limit: Int = 97) throws {
        guard [generation, course].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }),
              hash.count == 32, offset >= 0, offset < CourseBaselineReview.maximumSize,
              limit > 0, limit <= CourseBaselineReviewPage.maximumBytes,
              hash.contains(where: { $0 != 0 }) || offset == 0 else { throw ProtocolError.value }
        self.generation = generation
        self.course = course
        self.hash = hash
        self.offset = offset
        self.limit = limit
    }
    public func encode() -> Data {
        var bytes = Data([0x54, 0x43, 0x42, 0x51, 1, 0])
        bytes.append(generation)
        bytes.append(course)
        bytes.append(hash)
        bytes.appendLittleEndian(UInt64(offset), count: 2)
        bytes.appendLittleEndian(UInt64(limit), count: 2)
        return bytes
    }
    public func frame(requestID: UInt32) throws -> ControlFrame {
        try ControlFrame(command: .courseBaselineReview, requestID: requestID, payload: encode())
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == 74 else { throw ProtocolError.length }
        var cursor = ByteReader(bytes)
        guard try cursor.take(6) == Data([0x54, 0x43, 0x42, 0x51, 1, 0]) else { throw ProtocolError.version }
        try self.init(generation: cursor.take(16), course: cursor.take(16), hash: cursor.take(32),
                      offset: Int(cursor.number(2)), limit: Int(cursor.number(2)))
    }
}

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

    public static func decode(_ response: ControlFrame, request: ControlFrame) throws -> Self {
        guard request.command == .courseBaselineReview, !request.response, response.response,
              response.requestID == request.requestID else { throw ProtocolError.value }
        let expected = try CourseBaselineReviewPageRequest(decoding: request.payload)
        if response.command == .error {
            guard response.payload.count == 1, let raw = response.payload.first,
                  let error = CourseBaselineReviewControlError(rawValue: raw) else { throw ProtocolError.value }
            throw error
        }
        guard response.command == .courseBaselineReview else { throw ProtocolError.command }
        let page = try Self(decoding: response.payload)
        guard page.offset == expected.offset,
              page.bytes.count == min(expected.limit, page.total - page.offset),
              !expected.hash.contains(where: { $0 != 0 }) || page.hash == expected.hash else {
            throw ProtocolError.value
        }
        return page
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

    public func nextRequest(limit: Int = CourseBaselineReviewPage.maximumBytes) throws -> CourseBaselineReviewPageRequest {
        guard completed == nil else { throw ProtocolError.value }
        return try CourseBaselineReviewPageRequest(generation: generation, course: course,
            hash: hash ?? Data(repeating: 0, count: 32), offset: buffer.count, limit: limit)
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

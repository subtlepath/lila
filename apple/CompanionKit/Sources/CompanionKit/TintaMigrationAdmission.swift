import Foundation

/// A declaration; reader admission additionally verifies the immutable backup
/// and excludes learner/journal writers through publication and recovery.
public struct TintaMigrationAdmission: Equatable, Sendable {
    public static let byteCount = 256
    public let merge: JournalMergeDeclaration
    public let course: Data
    public let resource: Data
    public let backupTransaction: Data
    public let reader: Data
    public let backupManifest: Data

    public init(merge: JournalMergeDeclaration, course: Data, resource: Data,
                backupTransaction: Data, reader: Data, backupManifest: Data) throws {
        guard merge.merged.count > merge.previous.count,
              [course, backupTransaction, reader].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }),
              [resource, backupManifest].allSatisfy({ $0.count == 32 && $0.contains(where: { $0 != 0 }) }) else {
            throw ProtocolError.value
        }
        self.merge = merge; self.course = course; self.resource = resource
        self.backupTransaction = backupTransaction; self.reader = reader; self.backupManifest = backupManifest
    }
    public var bytes: Data {
        var result = Data([0x54, 0x4d, 0x41, 1])
        result.reserveCapacity(Self.byteCount)
        result.append(merge.bytes); result.append(course); result.append(resource)
        result.append(backupTransaction); result.append(reader); result.append(backupManifest)
        result.appendLittleEndian(UInt64(legacyCRC32(result)), count: 4)
        return result
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == Self.byteCount else { throw ProtocolError.length }
        var cursor = ByteReader(bytes)
        guard try cursor.take(4) == Data([0x54, 0x4d, 0x41, 1]) else { throw ProtocolError.version }
        let merge = try JournalMergeDeclaration(decoding: cursor.take(136))
        let course = try cursor.take(16), resource = try cursor.take(32)
        let transaction = try cursor.take(16), reader = try cursor.take(16), manifest = try cursor.take(32)
        guard try cursor.number(4) == UInt64(legacyCRC32(bytes.prefix(252))) else { throw ProtocolError.value }
        try self.init(merge: merge, course: course, resource: resource,
                      backupTransaction: transaction, reader: reader, backupManifest: manifest)
    }
    public func frame(requestID: UInt32) throws -> ControlFrame {
        try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: bytes)
    }
}

public struct TintaMigrationAdmissionReply: Equatable, Sendable {
    public let result: JournalMergeResult
    public let count: UInt32
    public static func decode(_ reply: ControlFrame, request: ControlFrame) throws -> Self {
        guard reply.response, reply.requestID == request.requestID else { throw ReaderSessionError.invalidResponse }
        if reply.command == .error, reply.payload.count == 1 { throw ReaderSessionError.control(reply.payload[0]) }
        guard !request.response, request.command == .exchangeChanges,
              reply.command == .exchangeChanges, reply.payload.count == 24 else {
            throw ReaderSessionError.invalidResponse
        }
        let admission = try TintaMigrationAdmission(decoding: request.payload)
        var cursor = ByteReader(reply.payload)
        guard try cursor.number(1) == 1,
              let result = JournalMergeResult(rawValue: UInt8(try cursor.number(1))),
              try cursor.number(2) == 0, try cursor.take(16) == admission.merge.transaction else {
            throw ReaderSessionError.invalidResponse
        }
        let count = UInt32(try cursor.number(4))
        guard count == admission.merge.previous.count else { throw ReaderSessionError.invalidResponse }
        return Self(result: result, count: count)
    }
}

extension AuthenticatedReaderSession {
    public func admitTintaMigration(_ admission: TintaMigrationAdmission, requestID: UInt32) async throws -> TintaMigrationAdmissionReply {
        guard admission.reader == device.identity, admission.merge.owner == installation,
              admission.merge.generation == device.storageGeneration else { throw ReaderSessionError.wrongReader }
        let request = try admission.frame(requestID: requestID)
        return try TintaMigrationAdmissionReply.decode(await exchange(request), request: request)
    }
}

import Foundation

public struct JournalMergeSnapshot: Equatable, Sendable {
    public let count: UInt32
    public let recordSize: UInt16
    public let frontier: Data

    public init(count: UInt32, recordSize: UInt16, frontier: Data) throws {
        guard [512, 1024].contains(recordSize), count <= UInt32.max / UInt32(recordSize),
              frontier.count == 32, frontier.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        self.count = count; self.recordSize = recordSize; self.frontier = frontier
    }
    fileprivate var bytes: Data {
        var result = Data()
        result.reserveCapacity(40)
        result.appendLittleEndian(UInt64(count), count: 4)
        result.appendLittleEndian(UInt64(recordSize), count: 2)
        result.append(contentsOf: [0, 0])
        result.append(frontier)
        return result
    }
}

public struct JournalMergeDeclaration: Equatable, Sendable {
    public let generation: Data
    public let transaction: Data
    public let owner: Data
    public let previous: JournalMergeSnapshot
    public let merged: JournalMergeSnapshot

    public init(generation: Data, transaction: Data, owner: Data,
                previous: JournalMergeSnapshot, merged: JournalMergeSnapshot) throws {
        guard [generation, transaction, owner].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }),
              merged.recordSize == 1024, merged.count >= previous.count,
              merged.count != previous.count || merged.frontier == previous.frontier else { throw ProtocolError.value }
        self.generation = generation; self.transaction = transaction; self.owner = owner
        self.previous = previous; self.merged = merged
    }
    public var bytes: Data {
        var result = Data([0x4a, 0x4d, 0x50, 1])
        result.reserveCapacity(136)
        result.append(generation); result.append(owner); result.append(transaction)
        result.append(previous.bytes); result.append(merged.bytes)
        result.appendLittleEndian(UInt64(legacyCRC32(result)), count: 4)
        return result
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == 136 else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x4a, 0x4d, 0x50, 1]) else { throw ProtocolError.version }
        let generation = try reader.take(16), owner = try reader.take(16), transaction = try reader.take(16)
        func snapshot(_ reader: inout ByteReader) throws -> JournalMergeSnapshot {
            let count = UInt32(try reader.number(4)), stride = UInt16(try reader.number(2))
            guard try reader.number(2) == 0 else { throw ProtocolError.value }
            return try JournalMergeSnapshot(count: count, recordSize: stride, frontier: reader.take(32))
        }
        let previous = try snapshot(&reader), merged = try snapshot(&reader)
        guard try reader.number(4) == UInt64(legacyCRC32(bytes.prefix(132))) else { throw ProtocolError.value }
        try self.init(generation: generation, transaction: transaction, owner: owner, previous: previous, merged: merged)
    }
}

public enum JournalMergeRequest: Equatable, Sendable {
    case begin(JournalMergeDeclaration)
    case append(transaction: Data, mutation: JournalMutation)
    case commit(JournalMergeDeclaration)
    case abort(JournalMergeDeclaration)

    public var transaction: Data {
        switch self {
        case .begin(let declaration), .commit(let declaration), .abort(let declaration): return declaration.transaction
        case .append(let transaction, _): return transaction
        }
    }
    public static func decodePayload(_ payload: Data) throws -> Self {
        guard (28...1024).contains(payload.count) else { throw ProtocolError.length }
        var reader = ByteReader(payload)
        guard try reader.take(4) == Data([0x4a, 0x4d, 0x52, 1]) else { throw ProtocolError.version }
        let operation = try reader.number(1)
        guard (1...4).contains(operation), try reader.number(3) == 0 else { throw ProtocolError.value }
        let transaction = try reader.take(16)
        let envelope = Int(try reader.number(2)), body = Int(try reader.number(2))
        guard transaction.contains(where: { $0 != 0 }), payload.count == 28 + envelope + body else { throw ProtocolError.value }
        if operation == 2 {
            guard (165...293).contains(envelope), (1...668).contains(body) else { throw ProtocolError.length }
            let mutation = try JournalMutation(event: SyncEvent(decoding: reader.take(envelope)), body: reader.take(body))
            try mutation.validateReaderBody()
            return .append(transaction: transaction, mutation: mutation)
        }
        guard envelope == 0, body == 136 else { throw ProtocolError.length }
        let declaration = try JournalMergeDeclaration(decoding: reader.take(body))
        guard declaration.transaction == transaction else { throw ProtocolError.value }
        switch operation {
        case 1: return .begin(declaration)
        case 3: return .commit(declaration)
        default: return .abort(declaration)
        }
    }

    public func payload() throws -> Data {
        let operation: UInt8
        let transaction: Data
        let envelope: Data
        let body: Data
        switch self {
        case .begin(let declaration):
            operation = 1; transaction = declaration.transaction; envelope = Data(); body = declaration.bytes
        case .append(let identity, let mutation):
            guard identity.count == 16, identity.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
            try mutation.validateReaderBody()
            operation = 2; transaction = identity; envelope = mutation.event.bytes; body = mutation.body
        case .commit(let declaration):
            operation = 3; transaction = declaration.transaction; envelope = Data(); body = declaration.bytes
        case .abort(let declaration):
            operation = 4; transaction = declaration.transaction; envelope = Data(); body = declaration.bytes
        }
        guard body.count <= 668, envelope.count <= 293, 28 + envelope.count + body.count <= 1024 else {
            throw ProtocolError.length
        }
        var result = Data([0x4a, 0x4d, 0x52, 1, operation, 0, 0, 0])
        result.reserveCapacity(28 + envelope.count + body.count)
        result.append(transaction)
        result.appendLittleEndian(UInt64(envelope.count), count: 2)
        result.appendLittleEndian(UInt64(body.count), count: 2)
        result.append(envelope); result.append(body)
        return result
    }
    public func frame(requestID: UInt32) throws -> ControlFrame {
        try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: payload())
    }
}

public enum JournalMergeResult: UInt8, Sendable {
    case ok = 0, duplicate, invalid, conflict, corrupt, ioError, exhausted, unavailable
}

public struct JournalMergeReply: Equatable, Sendable {
    public let result: JournalMergeResult
    public let count: UInt32
    public static func decode(_ reply: ControlFrame, request: ControlFrame) throws -> Self {
        guard reply.response, reply.requestID == request.requestID else { throw ReaderSessionError.invalidResponse }
        if reply.command == .error, reply.payload.count == 1 { throw ReaderSessionError.control(reply.payload[0]) }
        guard reply.command == .exchangeChanges, reply.payload.count == 24, request.payload.count >= 28,
              request.payload.prefix(4) == Data([0x4a, 0x4d, 0x52, 1]) else { throw ReaderSessionError.invalidResponse }
        var reader = ByteReader(reply.payload)
        guard try reader.number(1) == 1, let result = JournalMergeResult(rawValue: UInt8(try reader.number(1))),
              try reader.number(2) == 0, try reader.take(16) == Data(request.payload[8..<24]) else {
            throw ReaderSessionError.invalidResponse
        }
        return Self(result: result, count: UInt32(try reader.number(4)))
    }
}

extension AuthenticatedReaderSession {
    public func exchangeJournalMerge(_ operation: JournalMergeRequest, requestID: UInt32) async throws -> JournalMergeReply {
        switch operation {
        case .begin(let declaration), .commit(let declaration), .abort(let declaration):
            guard declaration.owner == installation, declaration.generation == device.storageGeneration else {
                throw ReaderSessionError.wrongReader
            }
        case .append: break
        }
        let request = try operation.frame(requestID: requestID)
        return try JournalMergeReply.decode(await exchange(request), request: request)
    }
}

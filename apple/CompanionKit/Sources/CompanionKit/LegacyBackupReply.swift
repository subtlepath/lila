import Foundation

public struct LegacyBackupReply: Equatable, Sendable {
    public enum Result: UInt8, Sendable { case ok, invalid, conflict, ioError, missing, busy }
    public let result: Result
    public let final: Bool
    public let body: Data
    public init(decoding bytes: Data, request: LegacyBackupRequest) throws {
        guard bytes.count >= 32, bytes.count <= 800 else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x54, 0x4c, 0x53, 1]) else { throw ProtocolError.version }
        let resultByte = UInt8(try reader.number(1)), roleByte = UInt8(try reader.number(1))
        let finalByte = try reader.number(1)
        let roles: [LegacyBackupRole] = [.reviews, .items, .profile, .lessons, .readings, .starred, .usage, .days, .session]
        guard let result = Result(rawValue: resultByte), finalByte <= 1, try reader.number(1) == 0,
              roleByte == request.role.map({ UInt8(roles.firstIndex(of: $0)!) }) ?? 0xff,
              try reader.take(16) == request.transaction,
              try reader.number(4) == UInt64(request.offset) else { throw ProtocolError.value }
        let count = Int(try reader.number(2))
        guard try reader.number(2) == 0, count == bytes.count - 32,
              UInt64(request.offset) + UInt64(count) <= UInt32.max else { throw ProtocolError.length }
        let body = try reader.take(count)
        if result != .ok {
            guard count == 0, finalByte == 0 else { throw ProtocolError.value }
        } else {
            switch request.operation {
            case .capture, .close:
                guard count == 0, finalByte == 1 else { throw ProtocolError.value }
            case .manifest:
                guard count == 436, finalByte == 1 else { throw ProtocolError.value }
                let manifest = try LegacyReaderBackupManifest(decoding: body)
                guard manifest.transaction == request.transaction, manifest.manifest.course == request.course,
                      manifest.manifest.generation == request.generation else { throw ProtocolError.value }
            case .file:
                guard count <= request.count, finalByte == 1 || count == request.count else { throw ProtocolError.value }
            }
        }
        self.result = result; self.final = finalByte == 1; self.body = body
    }
}

extension LegacyBackupReply {
    public static func decode(_ reply: ControlFrame, request: ControlFrame) throws -> Self {
        guard reply.response, reply.requestID == request.requestID else { throw ReaderSessionError.invalidResponse }
        if reply.command == .error, reply.payload.count == 1 { throw ReaderSessionError.control(reply.payload[0]) }
        guard reply.command == .exchangeChanges, request.command == .exchangeChanges,
              !request.response else { throw ReaderSessionError.invalidResponse }
        return try Self(decoding: reply.payload, request: LegacyBackupRequest(decoding: request.payload))
    }
}

extension AuthenticatedReaderSession {
    public func exchangeLegacyBackup(_ operation: LegacyBackupRequest, requestID: UInt32) async throws -> LegacyBackupReply {
        guard operation.generation == device.storageGeneration else { throw ReaderSessionError.wrongReader }
        try Task.checkCancellation()
        let request = try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: operation.bytes)
        let reply = try LegacyBackupReply.decode(await exchange(request), request: request)
        if operation.operation == .manifest, reply.result == .ok {
            guard try LegacyReaderBackupManifest(decoding: reply.body).manifest.reader == device.identity else {
                throw ReaderSessionError.wrongReader
            }
        }
        return reply
    }
}

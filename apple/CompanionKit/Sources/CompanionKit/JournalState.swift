import Foundation

public enum JournalState {
    public static let requestSize = 20
    public static func request(generation: Data, requestID: UInt32) throws -> ControlFrame {
        guard generation.count == 16, generation.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        return try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: Data([0x4a, 0x53, 0x54, 1]) + generation)
    }
    public static func validateRequest(_ bytes: Data, generation: Data) throws {
        guard generation.count == 16, generation.contains(where: { $0 != 0 }),
              bytes.count == requestSize, bytes.prefix(4) == Data([0x4a, 0x53, 0x54, 1]),
              Data(bytes.dropFirst(4)) == generation else { throw ReaderSessionError.wrongReader }
    }
    public static func decode(_ reply: ControlFrame, request: ControlFrame) throws -> JournalMergeSnapshot {
        guard !request.response, request.command == .exchangeChanges, request.payload.count == requestSize,
              request.payload.prefix(4) == Data([0x4a, 0x53, 0x54, 1]),
              request.payload.dropFirst(4).contains(where: { $0 != 0 }),
              reply.response, reply.requestID == request.requestID else { throw ReaderSessionError.invalidResponse }
        if reply.command == .error, reply.payload.count == 1 { throw ReaderSessionError.control(reply.payload[0]) }
        guard reply.command == .exchangeChanges, reply.payload.count == 44 else { throw ReaderSessionError.invalidResponse }
        var reader = ByteReader(reply.payload)
        guard try reader.take(4) == Data([0x4a, 0x53, 0x53, 1]) else { throw ReaderSessionError.invalidResponse }
        let count = UInt32(try reader.number(4)), stride = UInt16(try reader.number(2))
        guard try reader.number(2) == 0 else { throw ReaderSessionError.invalidResponse }
        return try JournalMergeSnapshot(count: count, recordSize: stride, frontier: reader.take(32))
    }
}

extension AuthenticatedReaderSession {
    public func journalState(requestID: UInt32) async throws -> JournalMergeSnapshot {
        guard device.capabilities & JournalExportPage.capability != 0 else { throw ReaderSessionError.unsupportedProtocol }
        let request = try JournalState.request(generation: device.storageGeneration, requestID: requestID)
        return try JournalState.decode(await exchange(request), request: request)
    }
}

import Foundation

public enum JournalMergeReadiness: UInt8, Equatable, Sendable {
    case ready, migrationRequired, unavailable, noCourse

    public static let capability: UInt32 = 1 << 9
    public static let requestSize = 60

    public static func request(generation: Data, snapshot: JournalMergeSnapshot, requestID: UInt32) throws -> ControlFrame {
        guard generation.count == 16, generation.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        var payload = Data([0x4a, 0x52, 0x44, 1]) + generation
        payload.appendLittleEndian(UInt64(snapshot.count), count: 4)
        payload.appendLittleEndian(UInt64(snapshot.recordSize), count: 2)
        payload.append(contentsOf: [0, 0])
        payload.append(snapshot.frontier)
        return try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: payload)
    }
    public static func validateRequest(_ bytes: Data, generation: Data) throws {
        guard generation.count == 16, generation.contains(where: { $0 != 0 }), bytes.count == requestSize,
              bytes.prefix(4) == Data([0x4a, 0x52, 0x44, 1]), Data(bytes[4..<20]) == generation,
              bytes[26] == 0, bytes[27] == 0 else { throw ReaderSessionError.wrongReader }
        var reader = ByteReader(Data(bytes.dropFirst(20)))
        let count = UInt32(try reader.number(4)), stride = UInt16(try reader.number(2))
        _ = try reader.number(2)
        _ = try JournalMergeSnapshot(count: count, recordSize: stride, frontier: reader.take(32))
    }
    public static func decode(_ reply: ControlFrame, request: ControlFrame) throws -> Self {
        guard !request.response, request.command == .exchangeChanges, request.payload.count == 60,
              request.payload.prefix(4) == Data([0x4a, 0x52, 0x44, 1]),
              request.payload[26] == 0, request.payload[27] == 0,
              request.payload[4..<20].contains(where: { $0 != 0 }),
              reply.response, reply.requestID == request.requestID else { throw ReaderSessionError.invalidResponse }
        var snapshot = ByteReader(Data(request.payload.dropFirst(20)))
        let count = UInt32(try snapshot.number(4)), stride = UInt16(try snapshot.number(2))
        _ = try snapshot.number(2)
        _ = try JournalMergeSnapshot(count: count, recordSize: stride, frontier: snapshot.take(32))
        if reply.command == .error, reply.payload.count == 1 { throw ReaderSessionError.control(reply.payload[0]) }
        guard reply.command == .exchangeChanges, reply.payload.count == 8,
              reply.payload.prefix(4) == Data([0x4a, 0x52, 0x52, 1]),
              reply.payload.suffix(3) == Data(count: 3), let result = Self(rawValue: reply.payload[4]) else {
            throw ReaderSessionError.invalidResponse
        }
        return result
    }
}

extension AuthenticatedReaderSession {
    public func journalMergeReadiness(snapshot: JournalMergeSnapshot, requestID: UInt32) async throws -> JournalMergeReadiness {
        guard device.capabilities & (JournalExportPage.capability | JournalMergeReadiness.capability) ==
            (JournalExportPage.capability | JournalMergeReadiness.capability) else { throw ReaderSessionError.unsupportedProtocol }
        let request = try JournalMergeReadiness.request(generation: device.storageGeneration, snapshot: snapshot, requestID: requestID)
        return try JournalMergeReadiness.decode(await exchange(request), request: request)
    }
}

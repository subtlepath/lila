import Foundation

public struct JournalExportCursor: Equatable, Sendable {
    public static let start = JournalExportCursor(frontier: Data(count: 32), count: 0, next: 0)
    public let frontier: Data
    public let count: UInt32
    public let next: UInt32
    fileprivate init(frontier: Data, count: UInt32, next: UInt32) {
        self.frontier = frontier; self.count = count; self.next = next
    }
    public var payload: Data {
        var result = frontier
        result.reserveCapacity(40)
        result.appendLittleEndian(UInt64(count), count: 4)
        result.appendLittleEndian(UInt64(next), count: 4)
        return result
    }
}

public struct JournalExportPage: Equatable, Sendable {
    public static let capability: UInt32 = 1 << 3
    public let cursor: JournalExportCursor
    public let mutation: JournalMutation?
    public var complete: Bool { mutation == nil }

    public static func decode(_ reply: ControlFrame, requestID: UInt32, requested: JournalExportCursor) throws -> Self {
        guard reply.response, reply.requestID == requestID else { throw ReaderSessionError.invalidResponse }
        if reply.command == .error, reply.payload.count == 1 { throw ReaderSessionError.control(reply.payload[0]) }
        guard reply.command == .exchangeChanges else { throw ReaderSessionError.invalidResponse }
        return try Self(decoding: reply.payload, requested: requested)
    }

    public init(decoding payload: Data, requested: JournalExportCursor) throws {
        guard (48 ... 1024).contains(payload.count) else { throw ProtocolError.length }
        var reader = ByteReader(payload)
        guard try reader.number(1) == 1 else { throw ProtocolError.version }
        let flags = try reader.number(1)
        guard flags <= 1, try reader.number(2) == 0 else { throw ProtocolError.value }
        let count = UInt32(try reader.number(4)), next = UInt32(try reader.number(4))
        let frontier = try reader.take(32)
        let envelopeLength = Int(try reader.number(2)), bodyLength = Int(try reader.number(2))
        guard frontier.contains(where: { $0 != 0 }), next <= count,
              payload.count == 48 + envelopeLength + bodyLength, bodyLength <= 668 else {
            throw ProtocolError.value
        }
        if requested != .start {
            guard count == requested.count, frontier == requested.frontier else { throw HistoryError.staleFrontier }
        }
        if flags == 1 {
            guard envelopeLength == 0, bodyLength == 0, next == count, requested.next == count else {
                throw ProtocolError.value
            }
            mutation = nil
        } else {
            guard requested.next < count, next == requested.next + 1 else { throw ProtocolError.value }
            let event = try SyncEvent(decoding: reader.take(envelopeLength))
            let decoded = try JournalMutation(event: event, body: reader.take(bodyLength))
            try decoded.validateReaderBody()
            mutation = decoded
        }
        cursor = JournalExportCursor(frontier: frontier, count: count, next: next)
    }
}

extension JournalMutation {
    func validateReaderBody() throws {
        switch event.kind {
        case .readingPosition:
            _ = try ReadingAnchor(decoding: body)
            guard event.resource.contains(where: { $0 != 0 }), event.schedulerVersion == 0,
                  event.schedulerConfiguration == Data(count: 32) else { throw ProtocolError.value }
        case .bookmarkPut, .bookmarkDelete:
            _ = try BookmarkBody(mutation: self)
        case .preference:
            _ = try PreferenceBody(mutation: self)
        default:
            _ = try TintaBody(mutation: self)
        }
    }
}

extension AuthenticatedReaderSession {
    public func journalExportPage(cursor: JournalExportCursor = .start, requestID: UInt32) async throws -> JournalExportPage {
        guard device.capabilities & JournalExportPage.capability != 0 else { throw ReaderSessionError.unsupportedProtocol }
        let request = try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: cursor.payload)
        let reply = try await exchange(request)
        return try JournalExportPage.decode(reply, requestID: requestID, requested: cursor)
    }
}

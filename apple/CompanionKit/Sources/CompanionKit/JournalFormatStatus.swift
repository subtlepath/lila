import Foundation

public enum JournalFormatStatus {
    public static let capability: UInt32 = 1 << 2

    public static func decode(_ reply: ControlFrame, requestID: UInt32) throws -> [UInt8] {
        guard reply.response, reply.requestID == requestID, reply.command == .journalFormats else {
            throw ReaderSessionError.invalidResponse
        }
        if reply.payload.count == 1, let status = reply.payload.first, status != 0 {
            throw ReaderSessionError.control(status)
        }
        guard reply.payload.count == 2, reply.payload[0] == 0, reply.payload[1] & ~7 == 0 else {
            throw ReaderSessionError.invalidResponse
        }
        let mask = reply.payload[1]
        return (UInt8(1) ... 3).filter { mask & (1 << ($0 - 1)) != 0 }
    }
}

extension AuthenticatedReaderSession {
    public func journalHeaderVersions(requestID: UInt32) async throws -> [UInt8] {
        guard device.capabilities & JournalFormatStatus.capability != 0 else {
            throw ReaderSessionError.unsupportedProtocol
        }
        let request = try ControlFrame(command: .journalFormats, requestID: requestID)
        return try JournalFormatStatus.decode(await exchange(request), requestID: requestID)
    }
}

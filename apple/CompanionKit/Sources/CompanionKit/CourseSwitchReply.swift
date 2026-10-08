import Foundation

public enum CourseSwitchReply {
    public static func validate(_ reply: ControlFrame, request: ControlFrame) throws {
        guard !request.response, request.command == .exchangeChanges,
              reply.response, reply.requestID == request.requestID else { throw ReaderSessionError.invalidResponse }
        let consent = try CourseSwitchRequest(decoding: request.payload)
        if reply.command == .error, reply.payload.count == 1 { throw ReaderSessionError.control(reply.payload[0]) }
        guard reply.command == .exchangeChanges, reply.payload.count == 17,
              Data(reply.payload.dropFirst()) == consent.transaction,
              let result = TransferResult(rawValue: reply.payload[0]) else { throw ReaderSessionError.invalidResponse }
        guard result == .ok else { throw TransferCommandError.remote(result) }
    }
}

extension AuthenticatedReaderSession {
    public func authorizeCourseSwitch(_ consent: CourseSwitchRequest, requestID: UInt32) async throws {
        guard consent.generation == device.storageGeneration else { throw ReaderSessionError.wrongReader }
        try Task.checkCancellation()
        let request = try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: consent.bytes)
        try CourseSwitchReply.validate(await exchange(request), request: request)
    }
}

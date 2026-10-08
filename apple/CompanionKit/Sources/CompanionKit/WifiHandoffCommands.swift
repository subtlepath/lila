import Foundation

public enum WifiHandoffCommandError: Error, Equatable, Sendable {
    case unexpectedResponse, binding, control(UInt8)
}
public enum WifiHandoffCommands {
    public static func prepare(mode: WifiNetworkMode, transaction: Data, requestID: UInt32) throws -> ControlFrame {
        try validateIdentity(transaction)
        var bytes = Data([1, 1, mode.rawValue]); bytes.reserveCapacity(19); bytes.append(transaction)
        return try ControlFrame(command: .wifiHandoff, requestID: requestID, payload: bytes)
    }
    public static func activate(transaction: Data, session: Data, requestID: UInt32) throws -> ControlFrame {
        try sessionCommand(action: 2, transaction: transaction, session: session, requestID: requestID)
    }
    public static func cancel(transaction: Data, session: Data, requestID: UInt32) throws -> ControlFrame {
        try sessionCommand(action: 3, transaction: transaction, session: session, requestID: requestID)
    }
    public static func offer(_ reply: ControlFrame, to request: ControlFrame, reader: Data,
                             storageGeneration: Data, installation: Data) throws -> WifiNetworkOffer {
        guard request.command == .wifiHandoff, !request.response, request.payload.count == 19 else { throw ProtocolError.value }
        var input = ByteReader(request.payload)
        guard try input.number(1) == 1, try input.number(1) == 1,
              let mode = WifiNetworkMode(rawValue: UInt8(try input.number(1))) else { throw ProtocolError.value }
        let transaction = try input.take(16); try validateIdentity(transaction)
        try validateReply(reply, to: request)
        let offered = try WifiNetworkOffer(decoding: reply.payload)
        guard offered.mode == mode, offered.offer.matches(reader: reader, storageGeneration: storageGeneration,
            installation: installation, transaction: transaction) else { throw WifiHandoffCommandError.binding }
        return offered
    }
    public static func acknowledgement(_ reply: ControlFrame, to request: ControlFrame) throws {
        guard request.command == .wifiHandoff, !request.response, request.payload.count == 34 else { throw ProtocolError.value }
        var input = ByteReader(request.payload)
        guard try input.number(1) == 1 else { throw ProtocolError.version }
        let action = try input.number(1)
        guard action == 2 || action == 3 else { throw ProtocolError.value }
        try validateIdentity(input.take(16)); try validateIdentity(input.take(16))
        try validateReply(reply, to: request)
        guard reply.payload == Data([0]) else { throw WifiHandoffCommandError.unexpectedResponse }
    }
    private static func sessionCommand(action: UInt8, transaction: Data, session: Data, requestID: UInt32) throws -> ControlFrame {
        try validateIdentity(transaction); try validateIdentity(session)
        var bytes = Data([1, action]); bytes.reserveCapacity(34); bytes.append(transaction); bytes.append(session)
        return try ControlFrame(command: .wifiHandoff, requestID: requestID, payload: bytes)
    }
    private static func validateIdentity(_ value: Data) throws {
        guard value.count == 16, value.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
    }
    private static func validateReply(_ reply: ControlFrame, to request: ControlFrame) throws {
        guard reply.response, reply.requestID == request.requestID else { throw WifiHandoffCommandError.unexpectedResponse }
        if reply.command == .error {
            guard reply.payload.count == 1, let code = reply.payload.first else { throw WifiHandoffCommandError.unexpectedResponse }
            throw WifiHandoffCommandError.control(code)
        }
        guard reply.command == .wifiHandoff else { throw WifiHandoffCommandError.unexpectedResponse }
    }
}

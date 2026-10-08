import Foundation

public enum TransferResult: UInt8, Equatable, Sendable {
    case ok = 0, noTransaction, invalid, unauthorized, wrongStorage, busy, ioError, corrupt, offset, hashMismatch
}
public enum TransferCommandError: Error, Equatable, Sendable {
    case unexpectedResponse, identityMismatch, remote(TransferResult), control(UInt8)
}

public enum TransferCommands {
    public static let maximumChunk = 1000
    public static func begin(_ state: TransferState, requestID: UInt32) throws -> ControlFrame {
        guard state.phase == .receiving, state.durableOffset == 0 else { throw ProtocolError.value }
        let hex = state.contentHash.map { String(format: "%02x", $0) }.joined()
        let destination = Data(("/Books/Companion/" + hex + ".epub").utf8)
        var body = state.encoded(); body.reserveCapacity(100 + destination.count)
        body.append(UInt8(destination.count)); body.append(destination)
        return try ControlFrame(command: .beginTransfer, requestID: requestID, payload: body)
    }
    public static func begin(_ declaration: TransferDeclaration, requestID: UInt32) throws -> ControlFrame {
        let destination: String
        switch declaration.manifest.kind {
        case .epub: destination = "/Books/Companion/" + declaration.manifest.content.hex + ".epub"
        case .course: destination = "/tinta/course.pack"
        case .dictionary: destination = try DictionaryTransferPlan(manifest: declaration.manifest).destination
        case .firmware:
            guard declaration.manifest.formatVersion == 1, declaration.manifest.length >= 65536,
                  declaration.manifest.length <= UInt32.max else { throw TransferRunnerError.unsupportedContent }
            destination = "/Companion/firmware.bin"
        default: throw TransferRunnerError.unsupportedContent
        }
        let path = Data(destination.utf8)
        var body = declaration.encoded
        body.reserveCapacity(TransferDeclaration.encodedSize + 1 + path.count)
        body.append(UInt8(path.count)); body.append(path)
        return try ControlFrame(command: .beginTransfer, requestID: requestID, payload: body)
    }
    public static func beginFont(_ declaration: TransferDeclaration, plan: FontTransferPlan,
                                 requestID: UInt32) throws -> ControlFrame {
        guard declaration.manifest.kind == .font, declaration.manifest.formatVersion == plan.formatVersion else {
            throw TransferRunnerError.unsupportedContent
        }
        let path = Data(plan.destination.utf8)
        var body = declaration.encoded
        body.reserveCapacity(TransferDeclaration.encodedSize + 1 + path.count)
        body.append(UInt8(path.count)); body.append(path)
        return try ControlFrame(command: .beginTransfer, requestID: requestID, payload: body)
    }
    public static func chunk(transaction: Data, offset: UInt64, bytes: Data, requestID: UInt32) throws -> ControlFrame {
        try validateTransaction(transaction)
        guard !bytes.isEmpty, bytes.count <= maximumChunk, UInt64(bytes.count) <= UInt64.max - offset else {
            throw ProtocolError.value
        }
        var body = Data(); body.reserveCapacity(24 + bytes.count)
        body.append(transaction); body.appendLittleEndian(offset, count: 8); body.append(bytes)
        return try ControlFrame(command: .transferChunk, requestID: requestID, payload: body)
    }
    public static func transaction(_ command: Command, identity: Data, requestID: UInt32) throws -> ControlFrame {
        guard [.transferStatus, .commit, .abort].contains(command) else { throw ProtocolError.command }
        try validateTransaction(identity)
        return try ControlFrame(command: command, requestID: requestID, payload: identity)
    }
    public static func response(_ frame: ControlFrame, to request: ControlFrame, expected: TransferState) throws -> TransferState {
        guard frame.response, !request.response, frame.requestID == request.requestID,
              [.beginTransfer, .transferChunk, .transferStatus, .commit, .abort].contains(request.command) else {
            throw TransferCommandError.unexpectedResponse
        }
        if frame.command == .error {
            guard frame.payload.count == 1, let code = frame.payload.first else { throw ProtocolError.length }
            throw TransferCommandError.control(code)
        }
        guard frame.command == request.command else { throw TransferCommandError.unexpectedResponse }
        guard let first = frame.payload.first, let result = TransferResult(rawValue: first) else { throw ProtocolError.value }
        if result != .ok {
            guard frame.payload.count == 1 else { throw ProtocolError.length }
            throw TransferCommandError.remote(result)
        }
        let state = try TransferState(decoding: Data(frame.payload.dropFirst()))
        guard state.transaction == expected.transaction, state.owner == expected.owner,
              state.storageGeneration == expected.storageGeneration, state.contentHash == expected.contentHash,
              state.length == expected.length else { throw TransferCommandError.identityMismatch }
        if request.command == .commit && state.phase != .committed ||
            request.command == .abort && state.phase != .aborted ||
            request.command == .transferChunk && state.phase != .receiving {
            throw TransferCommandError.unexpectedResponse
        }
        return state
    }
    private static func validateTransaction(_ identity: Data) throws {
        guard identity.count == 16, identity.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
    }
}

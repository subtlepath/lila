import Foundation

public enum ReaderSessionError: Error, Equatable, Sendable {
    case busy, staleConnection, pairingRequired, wrongReader, unsupportedProtocol, invalidResponse, rejected, control(UInt8), requestIDsExhausted
}

// Implementations check the connection and enqueue the request atomically on their transport executor.
public protocol SessionTransport: CompanionTransport {
    func sessionIdentity() async throws -> UInt64
    func exchange(_ request: ControlFrame, connection: UInt64) async throws -> ControlFrame
}

public struct AuthenticatedReaderSession: CompanionTransport {
    public let device: DeviceDescriptor
    public let installation: Data
    private let transport: any SessionTransport
    private let connection: UInt64
    fileprivate init(connection: UInt64, device: DeviceDescriptor, credential: PairingCredential, transport: any SessionTransport) {
        self.connection = connection; self.device = device; installation = credential.installation; self.transport = transport
    }
    public func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        try await transport.exchange(request, connection: connection)
    }
}

public actor ReaderSession {
    private let credentials: PairingVault
    private var busy = false
    private var requestID: UInt32 = 0
    public init(credentials: PairingVault) { self.credentials = credentials }

    public func authenticate(transport: any SessionTransport, expectedReader: Data? = nil) async throws -> AuthenticatedReaderSession {
        try await open(transport: transport, expectedReader: expectedReader, pairing: false)
    }
    public func pair(transport: any SessionTransport, expectedReader: Data? = nil) async throws -> AuthenticatedReaderSession {
        try await open(transport: transport, expectedReader: expectedReader, pairing: true)
    }
    private func open(transport: any SessionTransport, expectedReader: Data?, pairing: Bool) async throws -> AuthenticatedReaderSession {
        guard !busy else { throw ReaderSessionError.busy }
        busy = true
        defer { busy = false }
        try Task.checkCancellation()
        let connection = try await transport.sessionIdentity()
        let discovery = try request(.discover)
        let descriptor = try DeviceDescriptor(decoding: await response(to: discovery, transport: transport, connection: connection))
        guard descriptor.identity.contains(where: { $0 != 0 }), descriptor.storageGeneration.contains(where: { $0 != 0 }) else {
            throw ProtocolError.value
        }
        if let expectedReader, expectedReader != descriptor.identity { throw ReaderSessionError.wrongReader }
        guard descriptor.minimumProtocol <= 1, descriptor.maximumProtocol >= 1 else { throw ReaderSessionError.unsupportedProtocol }
        let credential: PairingCredential
        if pairing { credential = try await credentials.preparePairing(for: descriptor.identity) }
        else {
            guard let stored = try await credentials.credential(for: descriptor.identity) else { throw ReaderSessionError.pairingRequired }
            credential = stored
        }
        do { try await authorize(.authenticateInstallation, credential: credential, transport: transport, connection: connection) }
        catch ReaderSessionError.rejected {
            guard pairing else { throw ReaderSessionError.rejected }
            try await authorize(.registerInstallation, credential: credential, transport: transport, connection: connection)
        }
        try Task.checkCancellation()
        guard try await transport.sessionIdentity() == connection else { throw ReaderSessionError.staleConnection }
        return AuthenticatedReaderSession(connection: connection, device: descriptor, credential: credential, transport: transport)
    }
    private func authorize(_ command: Command, credential: PairingCredential, transport: any SessionTransport, connection: UInt64) async throws {
        try Task.checkCancellation()
        let payload = try await response(to: request(command, payload: credential.authenticationPayload), transport: transport, connection: connection)
        guard payload == Data([0]) else { throw ReaderSessionError.invalidResponse }
    }
    private func request(_ command: Command, payload: Data = Data()) throws -> ControlFrame {
        guard requestID < UInt32.max else { throw ReaderSessionError.requestIDsExhausted }
        requestID += 1
        return try ControlFrame(command: command, requestID: requestID, payload: payload)
    }
    private func response(to request: ControlFrame, transport: any SessionTransport, connection: UInt64) async throws -> Data {
        let reply = try await transport.exchange(request, connection: connection)
        guard reply.response, reply.requestID == request.requestID else { throw ReaderSessionError.invalidResponse }
        if reply.command == .error {
            guard reply.payload.count == 1, let code = reply.payload.first else { throw ReaderSessionError.invalidResponse }
            if code == 2 { throw ReaderSessionError.rejected }
            throw ReaderSessionError.control(code)
        }
        guard reply.command == request.command else { throw ReaderSessionError.invalidResponse }
        return reply.payload
    }
}

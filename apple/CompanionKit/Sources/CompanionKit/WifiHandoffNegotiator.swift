import Foundation
import Dispatch

public enum WifiNegotiationError: Error, Equatable, Sendable {
    case busy, requestIDsExhausted
}

// Ephemeral: never persist or cloud-sync the offer. HTTP authentication confirms the switch.
public struct WifiHandoffNegotiation: Sendable {
    public let network: WifiNetworkOffer
    public let receivedAtNanoseconds: UInt64
    public let activationAcknowledged: Bool
}

public actor WifiHandoffNegotiator {
    private var busy = false
    private var requestID: UInt32 = 0
    public init() {}

    public func negotiate(session: AuthenticatedReaderSession, transaction: Data,
                          mode: WifiNetworkMode) async throws -> WifiHandoffNegotiation {
        try await negotiate(transport: session, reader: session.device.identity,
                            storageGeneration: session.device.storageGeneration,
                            installation: session.installation, transaction: transaction, mode: mode,
                            uncertainActivation: { error in
                                #if canImport(CoreBluetooth)
                                guard let error = error as? BluetoothTransportError else { return false }
                                switch error {
                                case .disconnected, .timeout: return true
                                default: return false
                                }
                                #else
                                return false
                                #endif
                            })
    }

    func negotiate(transport: any CompanionTransport, reader: Data, storageGeneration: Data,
                   installation: Data, transaction: Data, mode: WifiNetworkMode,
                   uncertainActivation: @Sendable (Error) -> Bool,
                   clock: @Sendable () -> UInt64 = { DispatchTime.now().uptimeNanoseconds }) async throws -> WifiHandoffNegotiation {
        guard !busy else { throw WifiNegotiationError.busy }
        guard requestID <= UInt32.max - 2 else { throw WifiNegotiationError.requestIDsExhausted }
        busy = true
        defer { busy = false }
        requestID += 2
        let prepare = try WifiHandoffCommands.prepare(mode: mode, transaction: transaction, requestID: requestID - 1)
        try Task.checkCancellation()
        // Conservatively include BLE round-trip time in the reader's initial budget.
        let receivedAt = clock()
        let reply = try await transport.exchange(prepare)
        let network = try WifiHandoffCommands.offer(reply, to: prepare, reader: reader,
                                                    storageGeneration: storageGeneration, installation: installation)
        func checkDeadline() throws {
            try Task.checkCancellation()
            let now = clock()
            guard now >= receivedAt,
                  now - receivedAt < UInt64(network.offer.lifetimeSeconds) * 1_000_000_000 else {
                throw WifiHandoffTransportError.expired
            }
        }
        try checkDeadline()
        let activate = try WifiHandoffCommands.activate(transaction: transaction, session: network.offer.session,
                                                       requestID: requestID)
        let acknowledgement: ControlFrame
        do {
            acknowledgement = try await transport.exchange(activate)
        } catch {
            try checkDeadline()
            guard uncertainActivation(error) else { throw error }
            return WifiHandoffNegotiation(network: network, receivedAtNanoseconds: receivedAt,
                                           activationAcknowledged: false)
        }
        try checkDeadline()
        try WifiHandoffCommands.acknowledgement(acknowledgement, to: activate)
        return WifiHandoffNegotiation(network: network, receivedAtNanoseconds: receivedAt,
                                       activationAcknowledged: true)
    }
}

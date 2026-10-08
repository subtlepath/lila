import Foundation
import Dispatch

@MainActor
public enum WifiHandoffConnector {
    #if canImport(Darwin)
    // Call only after authenticated BLE negotiation and the Wi-Fi transition.
    public static func prepare(offer: WifiHandoffOffer, reader: Data, storageGeneration: Data,
                               installation: Data, transaction: Data,
                               receivedAtNanoseconds: UInt64) async throws -> WifiHandoffTransport {
        try await prepare(offer: offer, reader: reader, storageGeneration: storageGeneration,
                          installation: installation, transaction: transaction,
                          receivedAtNanoseconds: receivedAtNanoseconds,
                          resolve: { offer, receivedAt in
                              try await WifiBonjourResolver.resolve(offer, receivedAtNanoseconds: receivedAt)
                          }, makeWire: { offer, address in
                              try WifiHTTPMessageTransport(offer: offer, resolvedAddress: address)
                          })
    }
    #endif

    static func prepare(offer: WifiHandoffOffer, reader: Data, storageGeneration: Data,
                        installation: Data, transaction: Data, receivedAtNanoseconds: UInt64,
                        resolve: @MainActor (WifiHandoffOffer, UInt64) async throws -> Data,
                        makeWire: @MainActor (WifiHandoffOffer, Data?) throws -> any WifiMessageTransport,
                        clock: @escaping @Sendable () -> UInt64 = { DispatchTime.now().uptimeNanoseconds }) async throws -> WifiHandoffTransport {
        guard offer.matches(reader: reader, storageGeneration: storageGeneration,
                            installation: installation, transaction: transaction) else {
            throw WifiHandoffTransportError.binding
        }
        func checkDeadline() throws {
            try Task.checkCancellation()
            let now = clock()
            guard now >= receivedAtNanoseconds,
                  now - receivedAtNanoseconds < UInt64(offer.lifetimeSeconds) * 1_000_000_000 else {
                throw WifiHandoffTransportError.expired
            }
        }
        try checkDeadline()
        var address: Data?
        if offer.requiresDiscovery {
            address = try await resolve(offer, receivedAtNanoseconds)
            try checkDeadline()
            guard let address, address.count == 4, address[address.startIndex] != 0,
                  address[address.startIndex] != 127, address[address.startIndex] < 224 else {
                throw WifiHTTPError.invalidRequest
            }
        }
        let wire = try makeWire(offer, address)
        do {
            try checkDeadline()
            return try WifiHandoffTransport(offer: offer, reader: reader, storageGeneration: storageGeneration,
                                            installation: installation, transaction: transaction,
                                            receivedAt: receivedAtNanoseconds, wire: wire, now: clock)
        } catch {
            await wire.close()
            throw error
        }
    }
}

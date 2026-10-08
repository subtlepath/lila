import Foundation
import Dispatch

public enum WifiManualJoinError: Error, Equatable, Sendable { case busy, finished }

// Transient UI guidance only. Confirmation does not authenticate a network/reader.
@MainActor
public final class WifiManualJoinRequest {
    public let ssid: String
    public let password: String?
    private let receivedAt: UInt64
    private let lifetime: UInt64
    private let clock: @Sendable () -> UInt64
    private var continuation: CheckedContinuation<Void, Error>?
    private var timer: Task<Void, Never>?
    private var finished = false
    private var cancelled = false
    public convenience init(_ negotiation: WifiHandoffNegotiation) throws {
        try self.init(network: negotiation.network, receivedAtNanoseconds: negotiation.receivedAtNanoseconds)
    }
    init(network: WifiNetworkOffer, receivedAtNanoseconds: UInt64,
         clock: @escaping @Sendable () -> UInt64 = { DispatchTime.now().uptimeNanoseconds }) throws {
        guard let ssid = String(data: network.ssid, encoding: .utf8) else { throw WifiHTTPError.invalidRequest }
        self.ssid = ssid
        password = network.mode == .hotspot ? String(data: network.password, encoding: .utf8) : nil
        receivedAt = receivedAtNanoseconds
        lifetime = UInt64(network.offer.lifetimeSeconds) * 1_000_000_000
        self.clock = clock
        _ = try remaining()
    }
    private func remaining() throws -> UInt64 {
        let now = clock()
        guard now >= receivedAt, now - receivedAt < lifetime else { throw WifiHandoffTransportError.expired }
        return lifetime - (now - receivedAt)
    }
    var awaitingConfirmation: Bool { continuation != nil }
    public func waitForConfirmation() async throws {
        if cancelled { throw CancellationError() }
        guard !finished else { throw WifiManualJoinError.finished }
        guard continuation == nil else { throw WifiManualJoinError.busy }
        try Task.checkCancellation()
        let budget = try remaining()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                self.continuation = continuation
                timer = Task { @MainActor [weak self] in
                    do { try await Task.sleep(nanoseconds: budget) } catch { return }
                    self?.finish(.failure(WifiHandoffTransportError.expired))
                }
            }
            try Task.checkCancellation()
            if cancelled { throw CancellationError() }
            _ = try remaining()
        } onCancel: {
            Task { @MainActor [weak self] in self?.cancel() }
        }
    }
    public func confirm() {
        guard continuation != nil else { return }
        do { _ = try remaining(); finish(.success(())) }
        catch { finish(.failure(error)) }
    }
    public func cancel() {
        cancelled = true
        finish(.failure(CancellationError()))
    }
    private func finish(_ result: Result<Void, Error>) {
        guard !finished, let continuation else { return }
        finished = true
        self.continuation = nil
        timer?.cancel(); timer = nil
        continuation.resume(with: result)
    }
}

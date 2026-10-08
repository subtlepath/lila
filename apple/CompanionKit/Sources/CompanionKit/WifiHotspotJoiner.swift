import Foundation
import Dispatch
#if os(iOS) && canImport(NetworkExtension)
@preconcurrency import NetworkExtension
#endif

@MainActor
protocol WifiHotspotDriver: AnyObject, Sendable {
    func apply(ssid: String, password: String, completion: @escaping @MainActor (Error?) -> Void)
    func remove()
}
@MainActor
private final class HotspotGate {
    enum State { case pending, applied, discarded }
    var state = State.pending
}
// Configuration application is not proof of connectivity or reader identity.
@MainActor
public final class WifiHotspotLease {
    private let driver: any WifiHotspotDriver
    private let gate: HotspotGate
    fileprivate init(driver: any WifiHotspotDriver, gate: HotspotGate) { self.driver = driver; self.gate = gate }
    public func close() {
        guard gate.state != .discarded else { return }
        gate.state = .discarded
        driver.remove()
    }
    deinit {
        let driver = driver, gate = gate
        Task { @MainActor in
            if gate.state != .discarded { gate.state = .discarded; driver.remove() }
        }
    }
}
public enum WifiHotspotJoinError: Error, Equatable, Sendable { case busy }
@MainActor
public enum WifiHotspotJoiner {
    private static weak var current: HotspotGate?
    #if os(iOS) && canImport(NetworkExtension)
    public static func apply(_ negotiation: WifiHandoffNegotiation) async throws -> WifiHotspotLease {
        try await apply(network: negotiation.network, receivedAtNanoseconds: negotiation.receivedAtNanoseconds,
                        driver: SystemHotspotDriver())
    }
    #endif
    static func apply(network: WifiNetworkOffer, receivedAtNanoseconds: UInt64,
                      driver: any WifiHotspotDriver,
                      clock: @escaping @Sendable () -> UInt64 = { DispatchTime.now().uptimeNanoseconds }) async throws -> WifiHotspotLease {
        guard network.mode == .hotspot else { throw WifiHTTPError.invalidRequest }
        guard current == nil || current?.state == .discarded else { throw WifiHotspotJoinError.busy }
        let operation = HotspotOperation(network: network, receivedAt: receivedAtNanoseconds, driver: driver, clock: clock)
        current = operation.gate
        return try await operation.run()
    }
}
@MainActor
private final class HotspotOperation {
    let network: WifiNetworkOffer
    let receivedAt: UInt64
    let driver: any WifiHotspotDriver
    let clock: @Sendable () -> UInt64
    let gate = HotspotGate()
    var continuation: CheckedContinuation<WifiHotspotLease, Error>?
    var timer: Task<Void, Never>?
    init(network: WifiNetworkOffer, receivedAt: UInt64, driver: any WifiHotspotDriver,
         clock: @escaping @Sendable () -> UInt64) {
        self.network = network; self.receivedAt = receivedAt; self.driver = driver; self.clock = clock
    }
    func remaining() throws -> UInt64 {
        try Task.checkCancellation()
        let now = clock(), budget = UInt64(network.offer.lifetimeSeconds) * 1_000_000_000
        guard now >= receivedAt, now - receivedAt < budget else { throw WifiHandoffTransportError.expired }
        return budget - (now - receivedAt)
    }
    func run() async throws -> WifiHotspotLease {
        let budget = try remaining()
        return try await withTaskCancellationHandler {
            let lease = try await withCheckedThrowingContinuation { continuation in
                self.continuation = continuation
                timer = Task { @MainActor [weak self] in
                    do { try await Task.sleep(nanoseconds: budget) } catch { return }
                    self?.finish(.failure(WifiHandoffTransportError.expired))
                }
                driver.apply(ssid: String(decoding: network.ssid, as: UTF8.self),
                             password: String(decoding: network.password, as: UTF8.self)) { [weak self, gate, driver] error in
                    if gate.state == .discarded { driver.remove(); return }
                    guard gate.state == .pending, let self else { return }
                    do {
                        _ = try self.remaining()
                        if let error { self.finish(.failure(error)) }
                        else { self.finish(.success(WifiHotspotLease(driver: driver, gate: gate))) }
                    } catch { self.finish(.failure(error)) }
                }
            }
            do { try Task.checkCancellation(); return lease }
            catch { lease.close(); throw error }
        } onCancel: {
            Task { @MainActor [weak self] in self?.finish(.failure(CancellationError())) }
        }
    }
    func finish(_ result: Result<WifiHotspotLease, Error>) {
        guard gate.state == .pending, let continuation else { return }
        self.continuation = nil
        timer?.cancel(); timer = nil
        switch result {
        case .success: gate.state = .applied
        case .failure: gate.state = .discarded; driver.remove()
        }
        continuation.resume(with: result)
    }
}
#if os(iOS) && canImport(NetworkExtension)
@MainActor
private final class SystemHotspotDriver: WifiHotspotDriver {
    private var ssid: String?
    func apply(ssid: String, password: String, completion: @escaping @MainActor (Error?) -> Void) {
        self.ssid = ssid
        let configuration = NEHotspotConfiguration(ssid: ssid, passphrase: password, isWEP: false)
        configuration.joinOnce = true
        NEHotspotConfigurationManager.shared.apply(configuration) { error in
            Task { @MainActor in
                if let error = error as NSError?, error.domain == NEHotspotConfigurationErrorDomain,
                   error.code == NEHotspotConfigurationError.alreadyAssociated.rawValue { completion(nil) }
                else { completion(error) }
            }
        }
    }
    func remove() {
        if let ssid { NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: ssid) }
    }
}
#endif

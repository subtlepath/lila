import Foundation

public enum WifiDiscoveryError: Error, Equatable, Sendable {
    case invalidOffer, expired, discovery, resolution
}

// NetService supplies Darwin sockaddr_in bytes, including network-order port.
func wifiDiscoveryIPv4(_ socket: Data, port: UInt16) -> Data? {
    guard socket.count == 16, socket[socket.startIndex] == 16,
          socket[socket.startIndex + 1] == 2 else { return nil }
    let bytes = Array(socket)
    guard (UInt16(bytes[2]) << 8 | UInt16(bytes[3])) == port,
          bytes[4] != 0, bytes[4] != 127, bytes[4] < 224 else { return nil }
    return Data(bytes[4..<8])
}

func wifiDiscoveryMatches(name: String, type: String, domain: String, expectedName: String) -> Bool {
    name == expectedName && type == WifiHandoffOffer.discoveryServiceType && domain == "local."
}

func wifiDiscoveryResolvedAddress(port: Int, addresses: [Data], expectedPort: UInt16) -> Data? {
    guard port == Int(expectedPort) else { return nil }
    for socket in addresses.prefix(16) {
        if let address = wifiDiscoveryIPv4(socket, port: expectedPort) { return address }
    }
    return nil
}

@MainActor
protocol WifiDiscoveryDriver: AnyObject {
    func start(name: String, type: String, port: UInt16, timeout: TimeInterval,
               completion: @escaping @MainActor (Result<Data, any Error>) -> Void)
    func stop()
}

@MainActor
public enum WifiBonjourResolver {
    #if canImport(Darwin)
    public static func resolve(_ offer: WifiHandoffOffer, receivedAtNanoseconds: UInt64) async throws -> Data {
        try await resolve(offer, receivedAtNanoseconds: receivedAtNanoseconds,
                          driver: FoundationWifiDiscoveryDriver())
    }
    #endif

    static func resolve(_ offer: WifiHandoffOffer, receivedAtNanoseconds: UInt64,
                        driver: any WifiDiscoveryDriver,
                        clock: @escaping @Sendable () -> UInt64 = { DispatchTime.now().uptimeNanoseconds }) async throws -> Data {
        guard offer.requiresDiscovery else { throw WifiDiscoveryError.invalidOffer }
        try Task.checkCancellation()
        let operation = WifiDiscoveryOperation(offer: offer, receivedAt: receivedAtNanoseconds,
                                               driver: driver, clock: clock)
        let result = try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in operation.start(continuation) }
        } onCancel: {
            Task { @MainActor in operation.finish(.failure(CancellationError())) }
        }
        try Task.checkCancellation()
        return result
    }
}

@MainActor
private final class WifiDiscoveryOperation {
    private let name: String
    private let port: UInt16
    private let receivedAt: UInt64
    private let lifetime: UInt64
    private let driver: any WifiDiscoveryDriver
    private let clock: @Sendable () -> UInt64
    private var continuation: CheckedContinuation<Data, any Error>?
    private var timer: Task<Void, Never>?
    private var completed = false
    private var earlyResult: Result<Data, any Error>?

    init(offer: WifiHandoffOffer, receivedAt: UInt64, driver: any WifiDiscoveryDriver,
         clock: @escaping @Sendable () -> UInt64) {
        name = offer.discoveryName; port = offer.port
        self.receivedAt = receivedAt; lifetime = UInt64(offer.lifetimeSeconds) * 1_000_000_000
        self.driver = driver; self.clock = clock
    }
    private var remaining: UInt64? {
        let now = clock()
        guard now >= receivedAt, now - receivedAt < lifetime else { return nil }
        return lifetime - (now - receivedAt)
    }
    func start(_ continuation: CheckedContinuation<Data, any Error>) {
        if let earlyResult { continuation.resume(with: earlyResult); return }
        self.continuation = continuation
        guard let remaining else { finish(.failure(WifiDiscoveryError.expired)); return }
        timer = Task { [weak self] in
            do { try await Task.sleep(nanoseconds: remaining) } catch { return }
            self?.finish(.failure(WifiDiscoveryError.expired))
        }
        driver.start(name: name, type: WifiHandoffOffer.discoveryServiceType, port: port,
                     timeout: Double(remaining) / 1_000_000_000) { [weak self] result in
            guard let self, !self.completed else { return }
            guard self.remaining != nil else { self.finish(.failure(WifiDiscoveryError.expired)); return }
            if case .success(let address) = result {
                guard address.count == 4, address[address.startIndex] != 0,
                      address[address.startIndex] != 127, address[address.startIndex] < 224 else {
                    self.finish(.failure(WifiDiscoveryError.resolution)); return
                }
            }
            self.finish(result)
        }
    }
    func finish(_ result: Result<Data, any Error>) {
        guard !completed else { return }
        completed = true
        timer?.cancel(); timer = nil
        driver.stop()
        if let continuation {
            self.continuation = nil
            continuation.resume(with: result)
        } else { earlyResult = result }
    }
}

#if canImport(Darwin)
// Main-run-loop scheduling keeps delegate callbacks on the main actor.
@MainActor
private final class FoundationWifiDiscoveryDriver: NSObject, @preconcurrency NetServiceBrowserDelegate,
                                                  @preconcurrency NetServiceDelegate, WifiDiscoveryDriver {
    private let browser = NetServiceBrowser()
    private var service: NetService?
    private var completion: (@MainActor (Result<Data, any Error>) -> Void)?
    private var expectedName = ""
    private var expectedPort: UInt16 = 0
    private var timeout: TimeInterval = 0

    func start(name: String, type: String, port: UInt16, timeout: TimeInterval,
               completion: @escaping @MainActor (Result<Data, any Error>) -> Void) {
        self.completion = completion
        expectedName = name; expectedPort = port; self.timeout = timeout
        browser.delegate = self
        browser.includesPeerToPeer = false
        browser.schedule(in: .main, forMode: .common)
        browser.searchForServices(ofType: type, inDomain: "local.")
    }
    func stop() {
        completion = nil
        browser.delegate = nil; browser.stop(); browser.remove(from: .main, forMode: .common)
        service?.delegate = nil; service?.stop(); service?.remove(from: .main, forMode: .common)
        service = nil
    }
    func netServiceBrowser(_ browser: NetServiceBrowser, didFind found: NetService, moreComing: Bool) {
        guard completion != nil, service == nil, wifiDiscoveryMatches(name: found.name, type: found.type, domain: found.domain, expectedName: expectedName) else { return }
        service = found
        found.delegate = self
        found.schedule(in: .main, forMode: .common)
        found.resolve(withTimeout: timeout)
    }
    func netServiceBrowser(_ browser: NetServiceBrowser, didNotSearch errorDict: [String: NSNumber]) {
        completion?(.failure(WifiDiscoveryError.discovery))
    }
    func netServiceDidResolveAddress(_ sender: NetService) {
        guard sender === service else { return }
        if let address = wifiDiscoveryResolvedAddress(port: sender.port, addresses: sender.addresses ?? [],
                                                       expectedPort: expectedPort) {
            completion?(.success(address))
        } else { completion?(.failure(WifiDiscoveryError.resolution)) }
    }
    func netService(_ sender: NetService, didNotResolve errorDict: [String: NSNumber]) {
        guard sender === service else { return }
        completion?(.failure(WifiDiscoveryError.resolution))
    }
}
#endif

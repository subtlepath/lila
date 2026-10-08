import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

public enum WifiHTTPError: Error, Equatable, Sendable {
    case closed, busy, invalidRequest, response, status(Int), length, redirect
}

// Lock protects URLSession callbacks, task cancellation and the timeout task.
final class WifiHTTPRequest: @unchecked Sendable {
    static let maximumBytes = WifiMessageCipher.maximumPayload + 31 + 16
    private let lock = NSLock()
    private var continuation: CheckedContinuation<Data, any Error>?
    private var task: URLSessionDataTask?
    private var timer: Task<Void, Never>?
    private var completed = false
    private var accepted = false
    private var expectedLength: Int64 = -1
    private var bytes = Data()

    func start(_ task: URLSessionDataTask, continuation: CheckedContinuation<Data, any Error>, timeout: UInt64) {
        let started = lock.withLock {
            guard !completed else { return false }
            self.task = task; self.continuation = continuation
            bytes.reserveCapacity(Self.maximumBytes)
            timer = Task { [self] in
                do { try await Task.sleep(nanoseconds: timeout) } catch { return }
                finish(.failure(URLError(.timedOut)))
            }
            return true
        }
        if started { task.resume() }
        else { task.cancel(); continuation.resume(throwing: CancellationError()) }
    }
    func cancel() { finish(.failure(CancellationError())) }
    func receive(_ response: URLResponse) -> Bool {
        guard let http = response as? HTTPURLResponse else { finish(.failure(WifiHTTPError.response)); return false }
        guard http.statusCode == 200 else { finish(.failure(WifiHTTPError.status(http.statusCode))); return false }
        guard http.mimeType?.lowercased() == "application/octet-stream" else { finish(.failure(WifiHTTPError.response)); return false }
        guard response.expectedContentLength <= Int64(Self.maximumBytes) else { finish(.failure(WifiHTTPError.length)); return false }
        return lock.withLock {
            guard !completed else { return false }
            accepted = true; expectedLength = response.expectedContentLength
            return true
        }
    }
    func receive(_ data: Data) {
        let invalid = lock.withLock {
            guard !completed else { return false }
            guard accepted, data.count <= Self.maximumBytes - bytes.count else { return true }
            bytes.append(data)
            return false
        }
        if invalid { finish(.failure(WifiHTTPError.length)) }
    }
    func complete(_ error: (any Error)?) {
        if let error { finish(.failure(error)); return }
        let result: Result<Data, any Error> = lock.withLock {
            guard accepted else { return .failure(WifiHTTPError.response) }
            guard bytes.count >= 48, expectedLength < 0 || bytes.count == expectedLength else { return .failure(WifiHTTPError.length) }
            return .success(bytes)
        }
        finish(result)
    }
    func finish(_ result: Result<Data, any Error>) {
        let pending = lock.withLock { () -> (CheckedContinuation<Data, any Error>?, URLSessionDataTask?, Task<Void, Never>?) in
            guard !completed else { return (nil, nil, nil) }
            completed = true
            let pending = (continuation, task, timer)
            continuation = nil; task = nil; timer = nil
            bytes.removeAll(keepingCapacity: false)
            return pending
        }
        pending.2?.cancel()
        if case .failure = result { pending.1?.cancel() }
        pending.0?.resume(with: result)
    }
}

final class WifiHTTPDelegate: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    private let lock = NSLock()
    private var requests: [Int: WifiHTTPRequest] = [:]
    func register(_ request: WifiHTTPRequest, task: URLSessionDataTask) {
        lock.withLock { requests[task.taskIdentifier] = request }
    }
    private func request(_ task: URLSessionTask) -> WifiHTTPRequest? { lock.withLock { requests[task.taskIdentifier] } }
    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                    completionHandler: @escaping @Sendable (URLSession.ResponseDisposition) -> Void) {
        completionHandler(request(dataTask)?.receive(response) == true ? .allow : .cancel)
    }
    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) { request(dataTask)?.receive(data) }
    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: (any Error)?) {
        let pending = lock.withLock { requests.removeValue(forKey: task.taskIdentifier) }
        pending?.complete(error)
    }
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void) {
        self.request(task)?.finish(.failure(WifiHTTPError.redirect))
        completionHandler(nil)
    }
    func urlSession(_ session: URLSession, task: URLSessionTask, didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping @Sendable (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        completionHandler(.cancelAuthenticationChallenge, nil)
    }
}

public actor WifiHTTPMessageTransport: WifiMessageTransport {
    private let endpoint: URL
    private let delegate: WifiHTTPDelegate
    private let session: URLSession
    private var active = true
    private var busy = false
    private var current: WifiHTTPRequest?

    public init(offer: WifiHandoffOffer, resolvedAddress: Data? = nil, configuration: URLSessionConfiguration = .ephemeral) throws {
        let selectedAddress: Data
        if offer.requiresDiscovery {
            guard let resolvedAddress, resolvedAddress.count == 4,
                  resolvedAddress[0] != 0, resolvedAddress[0] != 127, resolvedAddress[0] < 224 else {
                throw WifiHTTPError.invalidRequest
            }
            selectedAddress = resolvedAddress
        } else {
            guard resolvedAddress == nil else { throw WifiHTTPError.invalidRequest }
            selectedAddress = offer.address
        }
        let address = selectedAddress.map(String.init).joined(separator: ".")
        guard let endpoint = URL(string: "http://\(address):\(offer.port)/companion/v1/messages"),
              let config = configuration.copy() as? URLSessionConfiguration else { throw WifiHTTPError.invalidRequest }
        config.urlCache = nil; config.httpCookieStorage = nil; config.urlCredentialStorage = nil
        config.httpShouldSetCookies = false; config.requestCachePolicy = .reloadIgnoringLocalCacheData
        config.httpMaximumConnectionsPerHost = 1
        config.timeoutIntervalForResource = 30
        config.connectionProxyDictionary = [:]
        #if canImport(Darwin)
        config.waitsForConnectivity = false
        config.allowsCellularAccess = false
        #endif
        delegate = WifiHTTPDelegate()
        session = URLSession(configuration: config, delegate: delegate, delegateQueue: nil)
        self.endpoint = endpoint
    }
    deinit { session.invalidateAndCancel() }
    public func close() async {
        active = false
        current?.cancel()
        session.invalidateAndCancel()
    }
    public func exchange(_ message: Data, timeoutNanoseconds: UInt64) async throws -> Data {
        guard active else { throw WifiHTTPError.closed }
        guard !busy else { throw WifiHTTPError.busy }
        guard message.count >= 48, message.count <= WifiHTTPRequest.maximumBytes,
              timeoutNanoseconds > 0, timeoutNanoseconds <= 30_000_000_000 else { throw WifiHTTPError.invalidRequest }
        try Task.checkCancellation()
        busy = true
        let pending = WifiHTTPRequest()
        current = pending
        defer { current = nil; busy = false }
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"; request.httpBody = message
        request.timeoutInterval = Double(timeoutNanoseconds) / 1_000_000_000
        request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        request.setValue("application/octet-stream", forHTTPHeaderField: "Accept")
        let task = session.dataTask(with: request)
        delegate.register(pending, task: task)
        do {
            let bytes = try await withTaskCancellationHandler {
                try await withCheckedThrowingContinuation { continuation in
                    pending.start(task, continuation: continuation, timeout: timeoutNanoseconds)
                }
            } onCancel: { pending.cancel() }
            try Task.checkCancellation()
            guard active else { throw WifiHTTPError.closed }
            return bytes
        } catch {
            active = false
            session.invalidateAndCancel()
            throw error
        }
    }
}

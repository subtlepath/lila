import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

// Apple-side bounded response workspace; the lock covers callbacks and cancellation.
final class ReleaseHTTPRequest: @unchecked Sendable {
    private let maximumBytes: Int
    private let lock = NSLock()
    private var continuation: CheckedContinuation<Data, any Error>?
    private var task: URLSessionDataTask?
    private var completed = false, accepted = false
    private var expectedLength: Int64 = -1
    private var bytes = Data()
    init(maximumBytes: Int) { self.maximumBytes = maximumBytes }
    func start(_ task: URLSessionDataTask, continuation: CheckedContinuation<Data, any Error>) {
        let started = lock.withLock {
            guard !completed else { return false }
            self.task = task; self.continuation = continuation
            bytes.reserveCapacity(maximumBytes)
            return true
        }
        if started { task.resume() }
        else { task.cancel(); continuation.resume(throwing: CancellationError()) }
    }
    func cancel() { finish(.failure(CancellationError())) }
    func receive(_ response: URLResponse) -> Bool {
        guard let http = response as? HTTPURLResponse, http.statusCode == 200,
              http.url?.scheme == "https", response.expectedContentLength <= Int64(maximumBytes) else {
            finish(.failure(ReleaseDiscoveryError.response)); return false
        }
        return lock.withLock {
            guard !completed else { return false }
            accepted = true; expectedLength = response.expectedContentLength
            return true
        }
    }
    func receive(_ data: Data) {
        let invalid = lock.withLock {
            guard !completed else { return false }
            guard accepted, data.count <= maximumBytes - bytes.count else { return true }
            bytes.append(data)
            return false
        }
        if invalid { finish(.failure(ReleaseDiscoveryError.length)) }
    }
    func complete(_ error: (any Error)?) {
        if let error { finish(.failure(error)); return }
        let result: Result<Data, any Error> = lock.withLock {
            guard accepted else { return .failure(ReleaseDiscoveryError.response) }
            guard expectedLength < 0 || bytes.count == expectedLength else { return .failure(ReleaseDiscoveryError.length) }
            return .success(bytes)
        }
        finish(result)
    }
    func finish(_ result: Result<Data, any Error>) {
        let pending = lock.withLock { () -> (CheckedContinuation<Data, any Error>?, URLSessionDataTask?) in
            guard !completed else { return (nil, nil) }
            completed = true
            let pending = (continuation, task)
            continuation = nil; task = nil; bytes.removeAll(keepingCapacity: false)
            return pending
        }
        if case .failure = result { pending.1?.cancel() }
        pending.0?.resume(with: result)
    }
}
final class ReleaseHTTPDelegate: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    private let lock = NSLock()
    private var requests: [Int: ReleaseHTTPRequest] = [:]
    func register(_ request: ReleaseHTTPRequest, task: URLSessionDataTask) {
        lock.withLock { requests[task.taskIdentifier] = request }
    }
    private func request(_ task: URLSessionTask) -> ReleaseHTTPRequest? {
        lock.withLock { requests[task.taskIdentifier] }
    }
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
                    newRequest next: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void) {
        guard let url = next.url, url.scheme == "https", url.user == nil, url.password == nil else {
            request(task)?.finish(.failure(ReleaseDiscoveryError.response)); completionHandler(nil); return
        }
        completionHandler(next)
    }
}

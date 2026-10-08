import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import XCTest
@testable import CompanionKit

private final class HTTPFixtureState: @unchecked Sendable {
    enum Mode: Equatable { case good, status, type, declaredTooLarge, streamTooLarge, never, encrypted(Data) }
    private let lock = NSLock()
    private var mode: Mode = .good
    private var requests: [URLRequest] = []
    func reset(_ mode: Mode) { lock.withLock { self.mode = mode; requests = [] } }
    func receive(_ request: URLRequest) -> Mode { lock.withLock { requests.append(request); return mode } }
    func recorded() -> [URLRequest] { lock.withLock { requests } }
}
private final class HandoffHTTPProtocol: URLProtocol, @unchecked Sendable {
    static let fixture = HTTPFixtureState()
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        let mode = Self.fixture.receive(request)
        if mode == .never { return }
        var headers = ["Content-Type": mode == .type ? "text/html" : "application/octet-stream"]
        if mode == .good { headers["Content-Length"] = "48" }
        if case .encrypted(let body) = mode { headers["Content-Length"] = String(body.count) }
        if mode == .declaredTooLarge { headers["Content-Length"] = "9999" }
        let response = HTTPURLResponse(url: request.url!, statusCode: mode == .status ? 503 : 200,
            httpVersion: "HTTP/1.1", headerFields: headers)!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        if mode == .streamTooLarge {
            client?.urlProtocol(self, didLoad: Data(repeating: 3, count: 600))
            client?.urlProtocol(self, didLoad: Data(repeating: 3, count: 600))
        } else if case .encrypted(let body) = mode {
            client?.urlProtocol(self, didLoad: body)
        } else if mode != .declaredTooLarge { client?.urlProtocol(self, didLoad: Data(repeating: 3, count: 48)) }
        client?.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
}

final class WifiHTTPMessageTransportTests: XCTestCase {
    private func transport(_ mode: HTTPFixtureState.Mode) throws -> WifiHTTPMessageTransport {
        HandoffHTTPProtocol.fixture.reset(mode)
        var bytes = Data([1])
        for value: UInt8 in 1...5 { bytes.append(Data(repeating: value, count: 16)) }
        bytes.append(Data(0..<32)); bytes.append(contentsOf: [192, 168, 4, 1])
        bytes.appendLittleEndian(8080, count: 2); bytes.appendLittleEndian(30, count: 2)
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [HandoffHTTPProtocol.self]
        return try WifiHTTPMessageTransport(offer: WifiHandoffOffer(decoding: bytes), configuration: config)
    }
    @MainActor
    func testConnectorComposesDiscoveryHTTPAndAuthenticatedMessages() async throws {
        var bytes = Data([1])
        for value: UInt8 in 1...5 { bytes.append(Data(repeating: value, count: 16)) }
        bytes.append(Data(0..<32)); bytes.append(Data(repeating: 0, count: 4))
        bytes.appendLittleEndian(8080, count: 2); bytes.appendLittleEndian(30, count: 2)
        let offer = try WifiHandoffOffer(decoding: bytes)
        let reader = try WifiMessageCipher(key: offer.key, session: offer.session, sending: .readerToApple)
        let response = try ControlFrame(command: .transferStatus, response: true, requestID: 77)
        let encrypted = try await reader.seal(response.encoded())
        HandoffHTTPProtocol.fixture.reset(.encrypted(encrypted))
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [HandoffHTTPProtocol.self]
        let transport = try await WifiHandoffConnector.prepare(offer: offer, reader: offer.reader,
            storageGeneration: offer.storageGeneration, installation: offer.installation,
            transaction: offer.transaction, receivedAtNanoseconds: 0,
            resolve: { _, receivedAt in XCTAssertEqual(receivedAt, 0); return Data([192, 168, 4, 2]) },
            makeWire: { offer, address in
                try WifiHTTPMessageTransport(offer: offer, resolvedAddress: address, configuration: config)
            }, clock: { 0 })
        let request = try ControlFrame(command: .transferStatus, requestID: 77, payload: offer.transaction)
        let received = try await transport.exchange(request)
        XCTAssertEqual(received.command, response.command); XCTAssertEqual(received.requestID, response.requestID)
        let posted = try XCTUnwrap(HandoffHTTPProtocol.fixture.recorded().first)
        XCTAssertEqual(posted.url?.absoluteString, "http://192.168.4.2:8080/companion/v1/messages")
        let postedBytes = try XCTUnwrap(posted.httpBody)
        XCTAssertNotEqual(postedBytes, try request.encoded())
        let decoded = try ControlFrame(decoding: await reader.open(postedBytes), authenticated: true)
        XCTAssertEqual(decoded.command, .transferStatus); XCTAssertEqual(decoded.requestID, 77)
        XCTAssertEqual(decoded.payload, offer.transaction)
        await transport.close()
    }
    func testPostBoundedResponseAndExplicitClose() async throws {
        let transport = try transport(.good)
        let input = Data(repeating: 1, count: 48)
        let output = try await transport.exchange(input, timeoutNanoseconds: 1_000_000_000)
        XCTAssertEqual(output, Data(repeating: 3, count: 48))
        let request = try XCTUnwrap(HandoffHTTPProtocol.fixture.recorded().first)
        XCTAssertEqual(request.url?.absoluteString, "http://192.168.4.1:8080/companion/v1/messages")
        XCTAssertEqual(request.httpMethod, "POST")
        XCTAssertEqual(request.httpBody, input)
        XCTAssertEqual(request.value(forHTTPHeaderField: "Content-Type"), "application/octet-stream")
        await transport.close()
        do { _ = try await transport.exchange(input, timeoutNanoseconds: 1_000_000_000); XCTFail("closed transport") }
        catch { XCTAssertEqual(error as? WifiHTTPError, .closed) }
    }
    func testHTTPAndCollectionFailuresCloseTransport() async throws {
        for mode in [HTTPFixtureState.Mode.status, .type, .declaredTooLarge, .streamTooLarge] {
            let transport = try transport(mode)
            do {
                _ = try await transport.exchange(Data(repeating: 1, count: 48), timeoutNanoseconds: 1_000_000_000)
                XCTFail("invalid response")
            } catch {
                let expected: WifiHTTPError = mode == .status ? .status(503) : mode == .type ? .response : .length
                XCTAssertEqual(error as? WifiHTTPError, expected)
            }
            do { _ = try await transport.exchange(Data(repeating: 1, count: 48), timeoutNanoseconds: 1_000_000_000); XCTFail("failed transport") }
            catch { XCTAssertEqual(error as? WifiHTTPError, .closed) }
        }
    }
    func testTimeoutAndCancellationEndPendingRequest() async throws {
        let timeout = try transport(.never)
        do {
            _ = try await timeout.exchange(Data(repeating: 1, count: 48), timeoutNanoseconds: 20_000_000)
            XCTFail("timeout")
        } catch { XCTAssertEqual((error as? URLError)?.code, .timedOut) }
        let cancelled = try transport(.never)
        let work = Task { try await cancelled.exchange(Data(repeating: 1, count: 48), timeoutNanoseconds: 1_000_000_000) }
        try await Task.sleep(nanoseconds: 20_000_000)
        work.cancel()
        do { _ = try await work.value; XCTFail("cancellation") }
        catch { XCTAssertTrue(error is CancellationError) }
        do { _ = try await cancelled.exchange(Data(repeating: 1, count: 48), timeoutNanoseconds: 1_000_000_000); XCTFail("cancelled transport") }
        catch { XCTAssertEqual(error as? WifiHTTPError, .closed) }
    }
    func testInvalidMessageAndTimeoutNeverStartHTTP() async throws {
        let transport = try transport(.good)
        for (length, timeout): (Int, UInt64) in [(47, 1), (1084, 1), (48, 0), (48, 30_000_000_001)] {
            do { _ = try await transport.exchange(Data(repeating: 1, count: length), timeoutNanoseconds: timeout); XCTFail("invalid request") }
            catch { XCTAssertEqual(error as? WifiHTTPError, .invalidRequest) }
        }
        XCTAssertTrue(HandoffHTTPProtocol.fixture.recorded().isEmpty)
        await transport.close()
    }
    func testCancellationBeforeRequestStartResumesWithoutTraffic() async throws {
        let pending = WifiHTTPRequest()
        pending.cancel()
        let session = URLSession(configuration: .ephemeral)
        defer { session.invalidateAndCancel() }
        let task = session.dataTask(with: URL(string: "http://192.168.4.1:8080/companion/v1/messages")!)
        do {
            let _: Data = try await withCheckedThrowingContinuation { continuation in
                pending.start(task, continuation: continuation, timeout: 1_000_000_000)
            }
            XCTFail("cancelled registration")
        } catch { XCTAssertTrue(error is CancellationError) }
    }
    func testRedirectDelegateRefusesNewEndpointAndCompletesRequest() async throws {
        HandoffHTTPProtocol.fixture.reset(.never)
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [HandoffHTTPProtocol.self]
        let delegate = WifiHTTPDelegate()
        let session = URLSession(configuration: config, delegate: delegate, delegateQueue: nil)
        defer { session.invalidateAndCancel() }
        let original = URL(string: "http://192.168.4.1:8080/companion/v1/messages")!
        let task = session.dataTask(with: original)
        let pending = WifiHTTPRequest()
        delegate.register(pending, task: task)
        do {
            let _: Data = try await withCheckedThrowingContinuation { continuation in
                pending.start(task, continuation: continuation, timeout: 1_000_000_000)
                let response = HTTPURLResponse(url: original, statusCode: 302, httpVersion: "HTTP/1.1",
                    headerFields: ["Location": "http://192.168.4.2/"])!
                delegate.urlSession(session, task: task, willPerformHTTPRedirection: response,
                    newRequest: URLRequest(url: URL(string: "http://192.168.4.2/")!)) { request in
                        XCTAssertNil(request)
                    }
            }
            XCTFail("redirect")
        } catch { XCTAssertEqual(error as? WifiHTTPError, .redirect) }
    }
}

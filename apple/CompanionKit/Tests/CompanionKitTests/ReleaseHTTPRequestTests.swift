import XCTest
import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
@testable import CompanionKit

private final class IdleReleaseProtocol: URLProtocol, @unchecked Sendable {
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {}
    override func stopLoading() {}
}

final class ReleaseHTTPRequestTests: XCTestCase {
    private func run(length: Int?, chunks: [Data], cancelBeforeStart: Bool = false) async throws -> Data {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [IdleReleaseProtocol.self]
        let session = URLSession(configuration: configuration)
        defer { session.invalidateAndCancel() }
        let url = URL(string: "https://release.example/manifest")!
        let pending = ReleaseHTTPRequest(maximumBytes: 4)
        if cancelBeforeStart { pending.cancel() }
        return try await withCheckedThrowingContinuation { continuation in
            pending.start(session.dataTask(with: url), continuation: continuation)
            let headers = length.map { ["Content-Length": String($0)] }
            if pending.receive(HTTPURLResponse(url: url, statusCode: 200, httpVersion: "HTTP/1.1", headerFields: headers)!) {
                for chunk in chunks { pending.receive(chunk) }
            }
            pending.complete(nil)
            pending.cancel()
            pending.complete(nil)
        }
    }

    func testExactAndUnknownLengthResponses() async throws {
        let exact = try await run(length: 4, chunks: [Data([1, 2]), Data([3, 4])])
        XCTAssertEqual(exact, Data([1, 2, 3, 4]))
        let unknown = try await run(length: nil, chunks: [Data([1, 2])])
        XCTAssertEqual(unknown, Data([1, 2]))
    }

    func testRejectsDeclaredOverflowChunkOverflowAndTruncation() async {
        for (length, chunks, expected) in [
            (5, [Data()], ReleaseDiscoveryError.response),
            (nil, [Data([1, 2, 3]), Data([4, 5])], .length),
            (4, [Data([1, 2, 3])], .length)
        ] as [(Int?, [Data], ReleaseDiscoveryError)] {
            do {
                _ = try await run(length: length, chunks: chunks)
                XCTFail("Accepted invalid response")
            } catch {
                XCTAssertEqual(error as? ReleaseDiscoveryError, expected)
            }
        }
    }

    func testCancellationBeforeStartIgnoresLateCallbacks() async {
        do {
            _ = try await run(length: 4, chunks: [Data([1, 2, 3, 4])], cancelBeforeStart: true)
            XCTFail("Accepted cancelled request")
        } catch {
            XCTAssertTrue(error is CancellationError)
        }
    }
}

private final class FixtureReleaseProtocol: URLProtocol, @unchecked Sendable {
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        let url = request.url!
        let status = url.path == "/error" ? 503 : 200
        let response = HTTPURLResponse(url: url, statusCode: status, httpVersion: "HTTP/1.1", headerFields: nil)!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: Data([1, 2, 3]))
        client?.urlProtocol(self, didLoad: Data([4, 5]))
        client?.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
}

extension ReleaseHTTPRequestTests {
    func testTransportRoutesResponsesAndEnforcesBounds() async throws {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [FixtureReleaseProtocol.self]
        let transport = URLSessionReleaseDiscoveryTransport(configuration: configuration)
        let bytes = try await transport.fetch(URL(string: "https://release.example/valid")!, maximumBytes: 5)
        XCTAssertEqual(bytes, Data([1, 2, 3, 4, 5]))
        for (path, limit, expected) in [("/valid", 4, ReleaseDiscoveryError.length), ("/error", 5, .response)] {
            do {
                _ = try await transport.fetch(URL(string: "https://release.example" + path)!, maximumBytes: limit)
                XCTFail("Accepted invalid response")
            } catch {
                XCTAssertEqual(error as? ReleaseDiscoveryError, expected)
            }
        }
        let retry = try await transport.fetch(URL(string: "https://release.example/valid")!, maximumBytes: 5)
        XCTAssertEqual(retry, bytes)
    }
}

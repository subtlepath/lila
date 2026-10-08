import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

public enum ReleaseDiscoveryError: Error, Equatable, Sendable { case response, length, release, asset, binding }
public protocol ReleaseDiscoveryTransport: Sendable {
    func fetch(_ url: URL, maximumBytes: Int) async throws -> Data
}
public final class URLSessionReleaseDiscoveryTransport: ReleaseDiscoveryTransport, @unchecked Sendable {
    private let delegate = ReleaseHTTPDelegate()
    private let session: URLSession
    public convenience init() {
        self.init(configuration: .ephemeral)
    }
    init(configuration: URLSessionConfiguration) {
        configuration.timeoutIntervalForRequest = 30
        configuration.timeoutIntervalForResource = 60
        configuration.httpCookieStorage = nil
        configuration.urlCredentialStorage = nil
        session = URLSession(configuration: configuration, delegate: delegate, delegateQueue: nil)
    }
    deinit { session.invalidateAndCancel() }
    public func fetch(_ url: URL, maximumBytes: Int) async throws -> Data {
        guard url.scheme == "https", url.user == nil, url.password == nil,
              (1 ... 1048576).contains(maximumBytes) else { throw ReleaseDiscoveryError.response }
        let pending = ReleaseHTTPRequest(maximumBytes: maximumBytes)
        return try await withTaskCancellationHandler {
            try Task.checkCancellation()
            return try await withCheckedThrowingContinuation { continuation in
                var request = URLRequest(url: url)
                request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
                let task = session.dataTask(with: request)
                delegate.register(pending, task: task)
                pending.start(task, continuation: continuation)
            }
        } onCancel: { pending.cancel() }
    }
}
public actor StableReleaseDiscovery {
    private struct Release: Decodable {
        struct Asset: Decodable { let name: String, state: String; let size: UInt64; let browser_download_url: URL }
        let tag_name: String
        let draft: Bool, prerelease: Bool
        let assets: [Asset]
    }
    private let transport: any ReleaseDiscoveryTransport
    private let repository: String
    public init(repository: String = "subtlepath/lila",
                transport: any ReleaseDiscoveryTransport = URLSessionReleaseDiscoveryTransport()) {
        self.repository = repository; self.transport = transport
    }
    public func latest() async throws -> CompanionReleaseManifest {
        guard repository.range(of: "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$", options: .regularExpression) != nil else {
            throw ReleaseDiscoveryError.binding
        }
        let api = URL(string: "https://api.github.com/repos/\(repository)/releases/latest")!
        let response = try await transport.fetch(api, maximumBytes: 1048576)
        guard response.count <= 1048576 else { throw ReleaseDiscoveryError.length }
        let release = try JSONDecoder().decode(Release.self, from: response)
        guard !release.draft, !release.prerelease, release.tag_name.utf8.count <= 65,
              release.tag_name.range(of: "^v[0-9]+\\.[0-9]+\\.[0-9]+$", options: .regularExpression) != nil else {
            throw ReleaseDiscoveryError.release
        }
        let matches = release.assets.filter { $0.name == "companion-release.json" }
        guard matches.count == 1, let asset = matches.first, asset.state == "uploaded",
              (24 ... 262144).contains(asset.size),
              asset.browser_download_url.absoluteString ==
                "https://github.com/\(repository)/releases/download/\(release.tag_name)/companion-release.json" else {
            throw ReleaseDiscoveryError.asset
        }
        let bytes = try await transport.fetch(asset.browser_download_url, maximumBytes: Int(asset.size))
        try Task.checkCancellation()
        guard bytes.count == asset.size else { throw ReleaseDiscoveryError.length }
        let manifest = try CompanionReleaseManifest.decode(bytes, repository: repository)
        guard manifest.tag == release.tag_name else { throw ReleaseDiscoveryError.binding }
        return manifest
    }
}

import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

public enum FirmwareDownloadError: Error, Equatable, Sendable { case busy, invalidURL, response, length }
public protocol FirmwareDownloadTransport: Sendable {
    /// Returns an owned temporary file; the caller removes it after import.
    func download(_ url: URL, expectedLength: UInt64) async throws -> URL
}
private final class FirmwareDownloadDelegate: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    let limit: UInt64
    init(limit: UInt64) { self.limit = limit }
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didWriteData bytesWritten: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        if totalBytesWritten < 0 || UInt64(totalBytesWritten) > limit ||
            (totalBytesExpectedToWrite >= 0 && UInt64(totalBytesExpectedToWrite) > limit) { downloadTask.cancel() }
    }
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) { }
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void) {
        guard let url = request.url, url.scheme == "https", url.user == nil, url.password == nil else {
            completionHandler(nil); return
        }
        completionHandler(request)
    }
}
public final class URLSessionFirmwareDownloadTransport: FirmwareDownloadTransport, @unchecked Sendable {
    private let session: URLSession
    public init() {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 30
        configuration.timeoutIntervalForResource = 120
        configuration.httpCookieStorage = nil
        configuration.urlCredentialStorage = nil
        session = URLSession(configuration: configuration)
    }
    deinit { session.invalidateAndCancel() }
    public func download(_ url: URL, expectedLength: UInt64) async throws -> URL {
        guard url.scheme == "https", url.user == nil, url.password == nil, expectedLength >= 24,
              expectedLength <= 16 * 1024 * 1024 else { throw FirmwareDownloadError.invalidURL }
        let delegate = FirmwareDownloadDelegate(limit: expectedLength)
        let (file, response) = try await session.download(for: URLRequest(url: url), delegate: delegate)
        var retained = false
        defer { if !retained { try? FileManager.default.removeItem(at: file) } }
        try Task.checkCancellation()
        guard let http = response as? HTTPURLResponse, http.statusCode == 200, http.url?.scheme == "https" else {
            throw FirmwareDownloadError.response
        }
        let size = try FileManager.default.attributesOfItem(atPath: file.path)[.size] as? NSNumber
        guard size?.uint64Value == expectedLength,
              http.expectedContentLength < 0 || UInt64(http.expectedContentLength) == expectedLength else {
            throw FirmwareDownloadError.length
        }
        retained = true
        return file
    }
}
public actor FirmwareDownloader {
    private let importer: ContentImporter
    private let transport: any FirmwareDownloadTransport
    private var busy = false
    public init(importer: ContentImporter, transport: any FirmwareDownloadTransport = URLSessionFirmwareDownloadTransport()) {
        self.importer = importer; self.transport = transport
    }
    public func download(_ asset: CompanionReleaseManifest.FirmwareAsset) async throws -> LibraryContent {
        guard !busy else { throw FirmwareDownloadError.busy }
        busy = true
        defer { busy = false }
        try Task.checkCancellation()
        let temporary = try await transport.download(asset.url, expectedLength: asset.metadata.length)
        defer { try? FileManager.default.removeItem(at: temporary) }
        try Task.checkCancellation()
        return try await importer.importFirmware(temporary, asset: asset)
    }
}

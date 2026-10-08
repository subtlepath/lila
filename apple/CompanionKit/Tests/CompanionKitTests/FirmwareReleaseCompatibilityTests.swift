import Foundation
import XCTest
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

private actor ReleaseDiscoveryFixture: ReleaseDiscoveryTransport {
    let responses: [URL: Data]
    init(responses: [URL: Data]) { self.responses = responses }
    func fetch(_ url: URL, maximumBytes: Int) async throws -> Data {
        guard let bytes = responses[url], bytes.count <= maximumBytes else { throw ReleaseDiscoveryError.length }
        return bytes
    }
}

private actor FirmwareDownloadFixture: FirmwareDownloadTransport {
    let bytes: Data
    let expectedURL: URL
    private var latest: URL?
    private var held: Bool
    private var gate: CheckedContinuation<Void, Never>?
    private var started: CheckedContinuation<Void, Never>?
    init(bytes: Data, expectedURL: URL, held: Bool = false) {
        self.bytes = bytes; self.expectedURL = expectedURL; self.held = held
    }
    func waitForRequest() async {
        if latest != nil { return }
        await withCheckedContinuation { started = $0 }
    }
    func release() {
        held = false
        gate?.resume(); gate = nil
    }
    func download(_ url: URL, expectedLength: UInt64) async throws -> URL {
        guard url == expectedURL, expectedLength == bytes.count else { throw FirmwareDownloadError.response }
        let temporary = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try bytes.write(to: temporary)
        latest = temporary
        if held {
            await withCheckedContinuation { gate = $0; started?.resume(); started = nil }
        }
        return temporary
    }
    func temporaryFile() -> URL? { latest }
}

final class FirmwareReleaseCompatibilityTests: XCTestCase, @unchecked Sendable {
    private func asset(_ changes: [String: Any] = [:]) throws -> FirmwareReleaseCompatibility {
        try JSONDecoder().decode(FirmwareReleaseCompatibility.self, from: assetData(changes))
    }
    private func assetData(_ changes: [String: Any] = [:]) throws -> Data {
        var json: [String: Any] = ["kind": "firmware", "sha256": String(repeating: "a", count: 64), "length": 1024, "boardTags": ["x4"], "chipId": 5,
            "otaPartitionBytes": 6553600, "minimumBatteryPercent": 30,
            "stateSchema": ["minimum": 2, "maximum": 3],
            "companionProtocol": ["minimum": 1, "maximum": 1], "initialUpgradeRequired": false,
            "supportedJournalHeaderVersions": [1, 2, 3]]
        for (key, value) in changes { json[key] = value }
        return try JSONSerialization.data(withJSONObject: json)
    }
    private func device(board: Board = .x4, battery: UInt8 = 30) throws -> DeviceDescriptor {
        var bytes = Data([1, 1])
        bytes.append(Data(repeating: 1, count: 16)); bytes.append(Data(repeating: 2, count: 16))
        bytes.append(board.rawValue); bytes.appendLittleEndian(4, count: 4)
        bytes.append(contentsOf: [battery, 1, 1]); bytes.append(Data(repeating: 3, count: 32))
        return try DeviceDescriptor(decoding: bytes)
    }
    func testRequiresKnownCompatibleReaderStateAndBattery() throws {
        let candidate = try asset(), reader = try device()
        XCTAssertTrue(candidate.permits(device: reader, readerStateSchema: 2, readerPartitionBytes: 6553600, readerHeaderVersions: [2, 3]))
        XCTAssertFalse(candidate.permits(device: reader, readerStateSchema: nil, readerPartitionBytes: 6553600, readerHeaderVersions: [3]))
        XCTAssertFalse(candidate.permits(device: reader, readerStateSchema: 4, readerPartitionBytes: 6553600, readerHeaderVersions: [3]))
        XCTAssertFalse(candidate.permits(device: reader, readerStateSchema: 2, readerPartitionBytes: nil, readerHeaderVersions: [3]))
        XCTAssertFalse(candidate.permits(device: reader, readerStateSchema: 2, readerPartitionBytes: 4096, readerHeaderVersions: [3]))
        XCTAssertFalse(candidate.permits(device: reader, readerStateSchema: 2, readerPartitionBytes: 6553600, readerHeaderVersions: nil))
        XCTAssertFalse(candidate.permits(device: try device(battery: 29), readerStateSchema: 2, readerPartitionBytes: 6553600, readerHeaderVersions: [3]))
        XCTAssertFalse(candidate.permits(device: try device(board: .sticky), readerStateSchema: 2, readerPartitionBytes: 6553600, readerHeaderVersions: [3]))
    }
    func testRejectsContradictoryReleaseMetadata() throws {
        for changes: [String: Any] in [["sha256": "bad"], ["sha256": String(repeating: "A", count: 64)],
            ["sha256": String(repeating: "0", count: 64)], ["kind": "course"], ["chipId": 9], ["boardTags": ["x4", "sticky"]],
            ["length": 6553601], ["minimumBatteryPercent": 29], ["minimumBatteryPercent": 101],
            ["stateSchema": ["minimum": 3, "maximum": 2]],
            ["companionProtocol": ["minimum": 0, "maximum": 1]],
            ["initialUpgradeRequired": true], ["companionProtocol": NSNull()]] {
            XCTAssertThrowsError(try asset(changes))
        }
        let legacy = try asset(["initialUpgradeRequired": true, "companionProtocol": NSNull(),
                                "supportedJournalHeaderVersions": []])
        XCTAssertFalse(legacy.permits(device: try device(), readerStateSchema: 2,
                                     readerPartitionBytes: 6553600, readerHeaderVersions: []))
    }
    func testStreamedImageValidationChecksChipBoardChecksumAndTrailer() async throws {
        var image = Data(repeating: 0, count: 24)
        image[0] = 0xe9; image[1] = 1; image[12] = 5; image[23] = 1
        // Put the tag across the inspector's 16 KiB read boundary.
        var payload = Data(repeating: 0, count: 16380)
        payload.append(Data("CROSSPOINT-BOARD-V1:x4;".utf8))
        image.appendLittleEndian(0x3c000020, count: 4)
        image.appendLittleEndian(UInt64(payload.count), count: 4)
        image.append(payload)
        let checksum = payload.reduce(UInt8(0xef), ^)
        let padded = (image.count + 16) & ~15
        image.append(Data(repeating: 0, count: padded - image.count - 1)); image.append(checksum)
        image.append(Data(SHA256.hash(data: image)))
        let releaseHash = SHA256.hash(data: image).map { String(format: "%02x", $0) }.joined()
        let compatibility = try asset(["length": image.count, "sha256": releaseHash])
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        try image.write(to: url)
        XCTAssertEqual(try FirmwareImageInspector.inspect(url, compatibility: compatibility), Data(SHA256.hash(data: image)))
        let cancelled = Task {
            withUnsafeCurrentTask { $0?.cancel() }
            return try FirmwareImageInspector.inspect(url, compatibility: compatibility)
        }
        do { _ = try await cancelled.value; XCTFail("cancelled validation") }
        catch is CancellationError { }
        catch { XCTFail("unexpected cancellation error: \(error)") }
        XCTAssertEqual(try Data(contentsOf: url), image)

        let vaultRoot = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: vaultRoot) }
        let vault = try ContentVault(root: vaultRoot)
        let stored = try await vault.importFirmware(url, compatibility: compatibility)
        XCTAssertEqual(stored.id.hex, releaseHash)
        XCTAssertEqual(stored.length, UInt64(image.count))
        let repeated = try await vault.importFirmware(url, compatibility: compatibility)
        XCTAssertEqual(repeated, stored)
        let databaseURL = vaultRoot.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: databaseURL)
        let importer = ContentImporter(vault: vault, library: library)
        let releaseAsset = try assetData(["length": image.count, "sha256": releaseHash])
        let content = try await importer.importFirmware(url, releaseAsset: releaseAsset)
        XCTAssertEqual(content.id, stored.id)
        XCTAssertEqual(content.kind, .firmware)
        let selected = CompanionReleaseManifest.FirmwareAsset(name: "lila-0.1.0-x3-x4.bin",
            url: URL(string: "https://github.com/subtlepath/lila/releases/download/v0.1.0/lila-0.1.0-x3-x4.bin")!,
            metadata: compatibility)
        let selectedContent = try await importer.importFirmware(url, asset: selected)
        XCTAssertEqual(selectedContent.id, content.id)
        XCTAssertEqual(selectedContent.originalFilename, selected.name)
        XCTAssertEqual(selectedContent.title, "lila-0.1.0-x3-x4")
        let wire = FirmwareDownloadFixture(bytes: image, expectedURL: selected.url)
        let downloader = FirmwareDownloader(importer: importer, transport: wire)
        let downloaded = try await downloader.download(selected)
        XCTAssertEqual(downloaded.id, stored.id)
        let temporary = await wire.temporaryFile()
        XCTAssertFalse(FileManager.default.fileExists(atPath: try XCTUnwrap(temporary).path))
        var badDownload = image; badDownload[0] ^= 1
        let corruptWire = FirmwareDownloadFixture(bytes: badDownload, expectedURL: selected.url)
        let corruptDownloader = FirmwareDownloader(importer: importer, transport: corruptWire)
        do { _ = try await corruptDownloader.download(selected); XCTFail("corrupt downloaded image") }
        catch { XCTAssertEqual(error as? VaultError, .integrity) }
        let heldWire = FirmwareDownloadFixture(bytes: image, expectedURL: selected.url, held: true)
        let heldDownloader = FirmwareDownloader(importer: importer, transport: heldWire)
        let activeDownload = Task { try await heldDownloader.download(selected) }
        await heldWire.waitForRequest()
        do { _ = try await heldDownloader.download(selected); XCTFail("overlapping download") }
        catch { XCTAssertEqual(error as? FirmwareDownloadError, .busy) }
        activeDownload.cancel()
        await heldWire.release()
        do { _ = try await activeDownload.value; XCTFail("cancelled downloaded file") }
        catch is CancellationError { }
        catch { XCTFail("unexpected cancelled download error: \(error)") }
        let cancelledFile = await heldWire.temporaryFile()
        XCTAssertFalse(FileManager.default.fileExists(atPath: try XCTUnwrap(cancelledFile).path))
        let afterCancellation = try await heldDownloader.download(selected)
        XCTAssertEqual(afterCancellation.id, stored.id)
        let corruptTemporary = await corruptWire.temporaryFile()
        XCTAssertFalse(FileManager.default.fileExists(atPath: try XCTUnwrap(corruptTemporary).path))

        XCTAssertEqual(try JSONDecoder().decode(FirmwareReleaseCompatibility.self,
                                               from: JSONEncoder().encode(compatibility)), compatibility)

        let metadata = try await library.firmwareCompatibility(content.id)
        XCTAssertEqual(metadata, compatibility)
        let reopened = try LibraryStore(url: databaseURL)
        let restored = try await reopened.firmwareCompatibility(content.id)
        XCTAssertEqual(restored, compatibility)
        let admitted = try await reopened.admitFirmwareTransfer(content.id, device: device(),
            readerStateSchema: 2, readerPartitionBytes: 6553600, readerHeaderVersions: [2, 3], vault: vault)
        XCTAssertEqual(admitted, stored)
        do {
            _ = try await reopened.admitFirmwareTransfer(content.id, device: device(battery: 29),
                readerStateSchema: 2, readerPartitionBytes: 6553600, readerHeaderVersions: [2, 3], vault: vault)
            XCTFail("low battery admission")
        } catch { XCTAssertEqual(error as? FirmwareTransferAdmissionError, .incompatibleReader) }
        do {
            _ = try await reopened.admitFirmwareTransfer(content.id, device: device(),
                readerStateSchema: nil, readerPartitionBytes: 6553600, readerHeaderVersions: [3], vault: vault)
            XCTFail("unknown reader state admission")
        } catch { XCTAssertEqual(error as? FirmwareTransferAdmissionError, .incompatibleReader) }

        let conflict = try assetData(["length": image.count, "sha256": releaseHash, "minimumBatteryPercent": 50])
        do { try await library.putFirmware(content, releaseAsset: conflict); XCTFail("conflicting metadata") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let preserved = try await library.firmwareCompatibility(content.id)
        XCTAssertEqual(preserved, compatibility)


        let cases: [(Int, FirmwareImageError)] = [(0, .header), (1, .header), (12, .chip),
            (32 + 16380 + 19, .board), (padded - 1, .checksum), (image.count - 1, .hash)]
        for (offset, expected) in cases {
            var corrupt = image; corrupt[offset] ^= 1
            try corrupt.write(to: url)
            XCTAssertThrowsError(try FirmwareImageInspector.inspect(url, compatibility: compatibility)) {
                XCTAssertEqual($0 as? FirmwareImageError, expected)
            }
        }
        var checksumOnly = Data(image.dropLast(32)); checksumOnly[23] = 0
        try checksumOnly.write(to: url)
        let noTrailer = try asset(["length": checksumOnly.count,
                                  "sha256": SHA256.hash(data: checksumOnly).map { String(format: "%02x", $0) }.joined()])
        XCTAssertEqual(try FirmwareImageInspector.inspect(url, compatibility: noTrailer),
                       Data(SHA256.hash(data: checksumOnly)))
        var otherRelease = image
        otherRelease[32] ^= 1
        otherRelease[padded - 1] ^= 1
        let otherTrailer = Data(SHA256.hash(data: otherRelease.prefix(padded)))
        otherRelease.replaceSubrange(padded ..< otherRelease.count, with: otherTrailer)
        try otherRelease.write(to: url)
        XCTAssertThrowsError(try FirmwareImageInspector.inspect(url, compatibility: compatibility)) {
            XCTAssertEqual($0 as? FirmwareImageError, .releaseHash)
        }
        do {
            _ = try await vault.importFirmware(url, compatibility: compatibility)
            XCTFail("different image must not enter the vault under the release identity")
        } catch { XCTAssertEqual(error as? VaultError, .integrity) }
        let retained = try await vault.verifiedObject(stored.id)
        XCTAssertEqual(retained, stored)
        XCTAssertEqual(try Data(contentsOf: retained.url), image)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: vaultRoot.appendingPathComponent("staging").path), [])
        try image.dropLast().write(to: url)
        XCTAssertThrowsError(try FirmwareImageInspector.inspect(url, compatibility: compatibility))
    }

    func testReleaseSelectionBindsRepositoryAndDefaultsToStable() async throws {
        func manifest(tag: String = "v0.1.0", channel: String = "stable", foreign: Bool = false) throws -> Data {
            var assets: [[String: Any]] = []
            for board in ["x4", "sticky", "x4pro", "x4c", "papermono"] {
                var value = try JSONSerialization.jsonObject(with: assetData(["boardTags": [board], "chipId": board == "x4" ? 5 : 9])) as! [String: Any]
                let name = "lila-\(tag.dropFirst())-\(board).bin"
                value["name"] = name
                value["url"] = "https://github.com/\(foreign ? "elsewhere/repo" : "subtlepath/lila")/releases/download/\(tag)/\(name)"
                assets.append(value)
            }
            return try JSONSerialization.data(withJSONObject: ["schemaVersion": 1, "version": String(tag.dropFirst()),
                "tag": tag, "channel": channel, "revision": String(repeating: "a", count: 40), "assets": assets])
        }
        let stable = try CompanionReleaseManifest.decode(manifest())
        XCTAssertEqual(stable.firmware(for: .x4)?.metadata.chipID, 5)
        XCTAssertEqual(stable.firmware(for: .sticky)?.metadata.chipID, 9)
        let api = URL(string: "https://api.github.com/repos/subtlepath/lila/releases/latest")!
        let location = URL(string: "https://github.com/subtlepath/lila/releases/download/v0.1.0/companion-release.json")!
        let payload = try manifest()
        func index(prerelease: Bool = false, duplicate: Bool = false) throws -> Data {
            let asset: [String: Any] = ["name": "companion-release.json", "state": "uploaded",
                "size": payload.count, "browser_download_url": location.absoluteString]
            return try JSONSerialization.data(withJSONObject: ["tag_name": "v0.1.0", "draft": false,
                "prerelease": prerelease, "assets": duplicate ? [asset, asset] : [asset]])
        }
        let discovery = StableReleaseDiscovery(transport: ReleaseDiscoveryFixture(responses: [api: try index(), location: payload]))
        let found = try await discovery.latest()
        XCTAssertEqual(found.tag, stable.tag)
        for badIndex in [try index(prerelease: true), try index(duplicate: true)] {
            let rejected = StableReleaseDiscovery(transport: ReleaseDiscoveryFixture(responses: [api: badIndex, location: payload]))
            do { _ = try await rejected.latest(); XCTFail("invalid discovery metadata") }
            catch { XCTAssertTrue(error is ReleaseDiscoveryError) }
        }

        XCTAssertThrowsError(try CompanionReleaseManifest.decode(manifest(foreign: true)))
        XCTAssertThrowsError(try CompanionReleaseManifest.decode(manifest(tag: "v0.1.0rc", channel: "rc")))
        XCTAssertNoThrow(try CompanionReleaseManifest.decode(manifest(tag: "v0.1.0rc", channel: "rc"), allowReleaseCandidates: true))
        XCTAssertThrowsError(try CompanionReleaseManifest.decode(manifest(tag: "v0.1.0rc", channel: "stable"), allowReleaseCandidates: true))
    }

}

import Foundation
import XCTest
@testable import CompanionKit

private enum RemovalFixtureError: Error { case disconnected }
private actor RemovalTransport: CompanionTransport {
    var requests: [Data] = []
    var loseReply = true
    var mismatchedReply = false
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        let removal = try ContentRemovalRequest(decoding: request.payload)
        requests.append(request.payload)
        if loseReply { loseReply = false; throw RemovalFixtureError.disconnected }
        return try ControlFrame(command: .removeContent, response: true, requestID: request.requestID + (mismatchedReply ? 1 : 0),
                                payload: Data([0]) + removal.transaction)
    }
    func bodies() -> [Data] { requests }
    func mismatch(_ value: Bool) { mismatchedReply = value }
}
final class ContentRemovalRunnerTests: XCTestCase, @unchecked Sendable {
    func testLostReplyRetriesTheSameDurableRequest() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        var bytes = Data([1, 1]); bytes.append(Data(repeating: 2, count: 16)); bytes.append(Data(repeating: 3, count: 16))
        bytes.append(1); bytes.appendLittleEndian(UInt64(ReaderCapabilities.epubRemovals.rawValue), count: 4)
        bytes.append(contentsOf: [80, 1, 1]); bytes.append(Data(count: 32))
        let device = try DeviceDescriptor(decoding: bytes)
        let owner = Data(repeating: 4, count: 16)
        let manifest = try ContentManifest(content: ContentID(String(repeating: "a", count: 64)), kind: .epub,
            length: 123, formatVersion: 1, logicalIdentity: Data(count: 16))
        let inventory = try ReaderInventory(reader: device.identity, generation: device.storageGeneration,
                                            contents: [manifest], complete: true)
        let job = try await library.queueRemoval(manifest: manifest, inventory: inventory, installation: owner)
        let runner = TransferRunner(library: library, vault: vault)
        let transport = RemovalTransport()
        var wrongBytes = device.encoded; wrongBytes[18] ^= 1
        let wrongDevice = try DeviceDescriptor(decoding: wrongBytes)
        do { _ = try await runner.removeContent(job.id, device: wrongDevice, installation: owner, transport: transport); XCTFail("Wrong card") }
        catch { XCTAssertEqual(error as? TransferRunnerError, .wrongStorage) }
        let untouched = try await library.removalJob(job.id)
        XCTAssertEqual(untouched?.phase, .queued)
        let unsent = await transport.bodies()
        XCTAssertTrue(unsent.isEmpty)
        do { _ = try await runner.removeContent(job.id, device: device, installation: owner, transport: transport); XCTFail("Reply lost") }
        catch { XCTAssertTrue(error is RemovalFixtureError) }
        let interrupted = try await library.removalJob(job.id)
        XCTAssertEqual(interrupted?.phase, .paused)
        await transport.mismatch(true)
        do { _ = try await runner.removeContent(job.id, device: device, installation: owner, transport: transport); XCTFail("Wrong response ID") }
        catch { XCTAssertEqual(error as? ProtocolError, .value) }
        let rejected = try await library.removalJob(job.id)
        XCTAssertEqual(rejected?.phase, .paused)
        await transport.mismatch(false)
        let completed = try await runner.removeContent(job.id, device: device, installation: owner, transport: transport)
        XCTAssertEqual(completed.phase, .completed)
        let repeated = try await runner.removeContent(job.id, device: device, installation: owner, transport: transport)
        XCTAssertEqual(repeated, completed)
        let bodies = await transport.bodies()
        XCTAssertEqual(bodies, [job.request.encoded, job.request.encoded, job.request.encoded])
    }
}

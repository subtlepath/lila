import Foundation
import XCTest
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
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
    func testDictionaryRemovalCapabilityIsIndependentOfTransferAndOtherRemovalKinds() {
        let otherCapabilities: ReaderCapabilities = [.declaredTransfers, .dictionaryTransfers, .epubRemovals, .fontRemovals]
        XCTAssertTrue(otherCapabilities.supportsDictionaryTransfer)
        XCTAssertFalse(otherCapabilities.supportsRemoval(of: .dictionary))
        XCTAssertTrue(ReaderCapabilities.dictionaryRemovals.supportsRemoval(of: .dictionary))
        XCTAssertFalse(ReaderCapabilities.dictionaryRemovals.supportsRemoval(of: .font))
        XCTAssertFalse(ReaderCapabilities.dictionaryRemovals.supportsRemoval(of: .epub))
        XCTAssertFalse(ReaderCapabilities.dictionaryRemovals.supportsRemoval(of: .course))
    }

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
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let resumed = TransferRunner(library: reopened, vault: vault)
        let pending = try await reopened.pendingRemovalJobs()
        XCTAssertEqual(pending, [interrupted!])
        await transport.mismatch(true)
        do { _ = try await resumed.removeContent(job.id, device: device, installation: owner, transport: transport); XCTFail("Wrong response ID") }
        catch { XCTAssertEqual(error as? ProtocolError, .value) }
        let rejected = try await library.removalJob(job.id)
        XCTAssertEqual(rejected?.phase, .paused)
        await transport.mismatch(false)
        let completed = try await resumed.removeContent(job.id, device: device, installation: owner, transport: transport)
        XCTAssertEqual(completed.phase, .completed)
        let repeated = try await resumed.removeContent(job.id, device: device, installation: owner, transport: transport)
        XCTAssertEqual(repeated, completed)
        let remaining = try await reopened.pendingRemovalJobs()
        XCTAssertTrue(remaining.isEmpty)
        let bodies = await transport.bodies()
        XCTAssertEqual(bodies, [job.request.encoded, job.request.encoded, job.request.encoded])
    }
    func testContentRemovalRequiresItsOwnCapabilityAndRetainsDurableJobs() async throws {
        for (kind, format): (ContentKind, UInt32) in [(.font, 1), (.font, 4), (.dictionary, 1), (.course, 1)] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let libraryURL = root.appendingPathComponent("library.sqlite")
            let library = try LibraryStore(url: libraryURL)
            let vault = try ContentVault(root: root.appendingPathComponent("vault"))
            var bytes = Data([1, 1]); bytes.append(Data(repeating: 2, count: 16)); bytes.append(Data(repeating: 3, count: 16))
            bytes.append(1); bytes.appendLittleEndian(UInt64(ReaderCapabilities.epubRemovals.rawValue), count: 4)
            bytes.append(contentsOf: [80, 1, 1]); bytes.append(Data(count: 32))
            let oldDevice = try DeviceDescriptor(decoding: bytes)
            let owner = Data(repeating: 4, count: 16)
            let manifest = try ContentManifest(content: ContentID(String(repeating: "b", count: 64)), kind: kind,
                length: 123, formatVersion: format, logicalIdentity: kind == .course ? Data(repeating: 9, count: 16) : Data(count: 16))
            let content = LibraryContent(id: manifest.content, kind: kind, length: 123, title: "Family",
                originalFilename: format == 4 ? "Family_14.cpfont" : "Family.ttf")
            try await library.put(content)
            let learning: JournalMutation?
            if kind == .course {
                let body = try TintaBody(subject: TintaSubject(course: manifest.logicalIdentity, uid: 1), value: .star(true))
                let event = try SyncEvent(identity: EventIdentity(origin: owner, epoch: 1, sequence: 1),
                    storageGeneration: oldDevice.storageGeneration, kind: .star,
                    resource: manifest.content.digest, bodyHash: Data(SHA256.hash(data: body.encoded)))
                learning = try JournalMutation(event: event, body: body.encoded)
                _ = try await library.importEvents([learning!])
            } else { learning = nil }
            let inventory = try ReaderInventory(reader: oldDevice.identity, generation: oldDevice.storageGeneration,
                contents: [manifest], complete: true)
            let job = try await library.queueRemoval(manifest: manifest, inventory: inventory, installation: owner)
            let duplicate = try await library.queueRemoval(manifest: manifest, inventory: inventory, installation: owner)
            XCTAssertEqual(duplicate, job)
            let transport = RemovalTransport()
            let runner = TransferRunner(library: library, vault: vault)
            do {
                _ = try await runner.removeContent(job.id, device: oldDevice, installation: owner, transport: transport)
                XCTFail("EPUB capability authorized another content kind removal")
            } catch { XCTAssertEqual(error as? TransferRunnerError, .unsupportedContent) }
            let unsent = await transport.bodies(); XCTAssertTrue(unsent.isEmpty)
            let queued = try await library.removalJob(job.id); XCTAssertEqual(queued, job)
            if kind == .course {
                let incomplete: [ReaderCapabilities] = [
                    [.courseRemovals], [.courseTransfers, .courseRemovals],
                    [.courseContexts, .courseRemovals], [.courseTransfers, .courseContexts]
                ]
                for capabilities in incomplete {
                    var descriptor = bytes
                    for offset in 0..<4 { descriptor[35 + offset] = UInt8(truncatingIfNeeded: capabilities.rawValue >> (8 * offset)) }
                    do {
                        _ = try await runner.removeContent(job.id, device: DeviceDescriptor(decoding: descriptor),
                            installation: owner, transport: transport)
                        XCTFail("Incomplete course capability admitted removal")
                    } catch { XCTAssertEqual(error as? TransferRunnerError, .unsupportedContent) }
                }
                let requests = await transport.bodies()
                XCTAssertTrue(requests.isEmpty)
            }
            let reopened = try LibraryStore(url: libraryURL)
            let resumed = TransferRunner(library: reopened, vault: vault)
            let capability: UInt32
            switch kind {
            case .course: capability = ReaderCapabilities([.courseTransfers, .courseContexts, .courseRemovals]).rawValue
            case .dictionary: capability = ReaderCapabilities.dictionaryRemovals.rawValue
            default: capability = ReaderCapabilities.fontRemovals.rawValue
            }
            for offset in 0..<4 { bytes[35 + offset] = UInt8(truncatingIfNeeded: capability >> (8 * offset)) }
            let compatible = try DeviceDescriptor(decoding: bytes)
            do {
                _ = try await resumed.removeContent(job.id, device: compatible, installation: owner, transport: transport)
                XCTFail("Lost reply ignored")
            } catch { XCTAssertTrue(error is RemovalFixtureError) }
            let completed = try await resumed.removeContent(job.id, device: compatible, installation: owner, transport: transport)
            XCTAssertEqual(completed.phase, .completed)
            let requests = await transport.bodies(); XCTAssertEqual(requests, [job.request.encoded, job.request.encoded])
            let retained = try await reopened.content(content.id); XCTAssertEqual(retained, content)
            let selected = try await reopened.isReaderContentSelected(reader: oldDevice.identity, content: content.id)
            XCTAssertFalse(selected)
            let globallyDeleted = try await reopened.isLibraryContentDeleted(content.id)
            XCTAssertFalse(globallyDeleted)
            if let learning {
                let retainedLearning = try await reopened.storedEvent(learning.event.identity)
                XCTAssertEqual(retainedLearning, learning)
            }
        }
    }

}

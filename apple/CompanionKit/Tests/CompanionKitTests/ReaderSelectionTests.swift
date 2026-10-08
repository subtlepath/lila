import Foundation
import XCTest
@testable import CompanionKit

final class ReaderSelectionTests: XCTestCase, @unchecked Sendable {
    func testSelectedJobCreationAcrossHandlesAndDeselection() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let first = try LibraryStore(url: url), second = try LibraryStore(url: url)
        let id = try ContentID(String(repeating: "c", count: 64))
        let reader = Data(repeating: 1, count: 16), generation = Data(repeating: 2, count: 16), owner = Data(repeating: 3, count: 16)
        try await first.put(LibraryContent(id: id, kind: .epub, length: 3, title: "Book", originalFilename: "book.epub"))
        do { _ = try await first.enqueueSelectedContent(content: id, reader: reader, storageGeneration: generation, installation: owner); XCTFail("Unselected content queued") }
        catch { XCTAssertEqual(error as? StoreError, .invalidTransition) }
        _ = try await first.setReaderSelection(reader: reader, content: id, selected: true)
        async let left = first.enqueueSelectedContent(content: id, reader: reader, storageGeneration: generation, installation: owner)
        async let right = second.enqueueSelectedContent(content: id, reader: reader, storageGeneration: generation, installation: owner)
        let (a, b) = try await (left, right)
        XCTAssertEqual(a.id, b.id)
        let pending = try await first.pendingJobs(); XCTAssertEqual(pending.count, 1)
        try await first.checkpoint(a.id, offset: 1, phase: .paused)
        let resumed = try await second.enqueueSelectedContent(content: id, reader: reader, storageGeneration: generation, installation: owner)
        XCTAssertEqual(resumed.id, a.id); XCTAssertEqual(resumed.durableOffset, 1)
        do { _ = try await second.enqueueSelectedContent(content: id, reader: reader, storageGeneration: generation, installation: Data(repeating: 4, count: 16)); XCTFail("Foreign transaction reused") }
        catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
        _ = try await second.setReaderSelection(reader: reader, content: id, selected: false)
        do { _ = try await first.enqueueSelectedContent(content: id, reader: reader, storageGeneration: generation, installation: owner); XCTFail("Stale selected plan queued") }
        catch { XCTAssertEqual(error as? StoreError, .invalidTransition) }
        let retained = try await first.job(a.id); XCTAssertEqual(retained?.phase, .paused)
    }
    func testIndependentSelectionsRestartAndRemovalRetainsLibrary() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: url)
        let id = try ContentID(String(repeating: "a", count: 64))
        let content = LibraryContent(id: id, kind: .epub, length: 3, title: "Book", originalFilename: "book.epub")
        try await library.put(content)
        let first = Data(repeating: 1, count: 16), second = Data(repeating: 2, count: 16)
        let added = try await library.setReaderSelection(reader: first, content: id, selected: true)
        XCTAssertTrue(added)
        let repeated = try await library.setReaderSelection(reader: first, content: id, selected: true)
        XCTAssertFalse(repeated)
        _ = try await library.setReaderSelection(reader: second, content: id, selected: true)
        _ = try await library.setReaderSelection(reader: first, content: id, selected: false)
        let reopened = try LibraryStore(url: url)
        let one = try await reopened.readerSelections(reader: first)
        let two = try await reopened.readerSelections(reader: second)
        XCTAssertEqual(one, [ReaderContentSelection(content: id, selected: false)])
        XCTAssertEqual(two, [ReaderContentSelection(content: id, selected: true)])
        let retained = try await reopened.content(id); XCTAssertEqual(retained, content)
        var manifestBytes = Data([1, 2]); manifestBytes.append(id.digest); manifestBytes.append(1)
        manifestBytes.appendLittleEndian(3, count: 8); manifestBytes.appendLittleEndian(1, count: 4)
        manifestBytes.append(Data(count: 16))
        let manifest = try ContentManifest(decoding: manifestBytes)
        let generation = Data(repeating: 3, count: 16)
        let inventory = try ReaderInventory(reader: first, generation: generation, contents: [manifest, manifest], complete: true)
        XCTAssertEqual(inventory.contents.count, 1)
        let removal = try await reopened.reconcileContent(reader: first, generation: generation, inventory: inventory)
        XCTAssertEqual(removal, [.remove(manifest)])
        let absent = try ReaderInventory(reader: second, generation: generation, contents: [], complete: true)
        let installation = try await reopened.reconcileContent(reader: second, generation: generation, inventory: absent)
        XCTAssertEqual(installation, [.install(content)])
        let present = try ReaderInventory(reader: second, generation: generation, contents: [manifest], complete: true)
        let unchanged = try await reopened.reconcileContent(reader: second, generation: generation, inventory: present)
        XCTAssertTrue(unchanged.isEmpty)
        let partial = try ReaderInventory(reader: first, generation: generation, contents: [], complete: false)
        do { _ = try await reopened.reconcileContent(reader: first, generation: generation, inventory: partial); XCTFail("Partial inventory reconciled") }
        catch { XCTAssertEqual(error as? ContentReconciliationError, .incompleteInventory) }
        do { _ = try await reopened.reconcileContent(reader: first, generation: Data(repeating: 4, count: 16), inventory: inventory); XCTFail("Wrong SD generation reconciled") }
        catch { XCTAssertEqual(error as? ContentReconciliationError, .wrongReader) }
        let owner = Data(repeating: 5, count: 16)
        let queued = try await reopened.enqueue(content: id, reader: second, storageGeneration: generation, installation: owner)
        try await reopened.checkpoint(queued.id, offset: 1, phase: .paused)
        let pausedValue = try await reopened.job(queued.id)
        let paused = try XCTUnwrap(pausedValue)
        let resumed = try await reopened.reconcileContentWork(reader: second, generation: generation, installation: owner, inventory: absent)
        XCTAssertEqual(resumed, [.resume(paused)])
        _ = try await reopened.setReaderSelection(reader: second, content: id, selected: false)
        let cancel = try await reopened.reconcileContentWork(reader: second, generation: generation, installation: owner, inventory: present)
        XCTAssertEqual(cancel, [.abort(paused)])
        try await reopened.checkpoint(queued.id, offset: 3, phase: .committing)
        let committingValue = try await reopened.job(queued.id)
        let committing = try XCTUnwrap(committingValue)
        let recover = try await reopened.reconcileContentWork(reader: second, generation: generation, installation: owner, inventory: present)
        XCTAssertEqual(recover, [.recoverCommit(committing)])
        let foreign = try await reopened.reconcileContentWork(reader: second, generation: generation,
            installation: Data(repeating: 6, count: 16), inventory: present)
        XCTAssertEqual(foreign, [.inspect(committing)])
        let changedCard = Data(repeating: 7, count: 16)
        let changedInventory = try ReaderInventory(reader: second, generation: changedCard, contents: [], complete: true)
        let stale = try await reopened.reconcileContentWork(reader: second, generation: changedCard,
            installation: owner, inventory: changedInventory)
        XCTAssertEqual(stale, [.staleGeneration(committing)])
        let duplicateRemoval = try await reopened.setReaderSelection(reader: first, content: id, selected: false)
        XCTAssertFalse(duplicateRemoval)
        do { _ = try await reopened.setReaderSelection(reader: Data(count: 16), content: id, selected: true); XCTFail("Invalid reader accepted") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let firmware = try ContentID(String(repeating: "b", count: 64))
        try await library.put(LibraryContent(id: firmware, kind: .firmware, length: 4, title: "Firmware", originalFilename: "firmware.bin"))
        do { _ = try await library.setReaderSelection(reader: first, content: firmware, selected: true); XCTFail("Implicit firmware installation allowed") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
    }
}

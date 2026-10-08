import Foundation
import XCTest
@testable import CompanionKit

final class SavedReaderTests: XCTestCase, @unchecked Sendable {
    private func fixture() throws -> Data {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { root.deleteLastPathComponent() }
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf:
            root.appendingPathComponent("protocol/fixtures/DeviceDescriptor.json"))) as! [String: Any]
        let hex = json["binaryHex"] as! String
        var result = Data(); result.reserveCapacity(74)
        var cursor = hex.startIndex
        while cursor < hex.endIndex {
            let end = hex.index(cursor, offsetBy: 2)
            result.append(UInt8(hex[cursor ..< end], radix: 16)!); cursor = end
        }
        return result
    }
    func testReaderDescriptorPersistsAndNewCardUpdatesSamePhysicalReader() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: url)
        let bytes = try fixture(), first = try DeviceDescriptor(decoding: bytes)
        XCTAssertEqual(first.encoded, bytes)
        try await library.saveReader(first, at: Date(timeIntervalSince1970: 100))
        let reopened = try LibraryStore(url: url, random: { Data(repeating: 7, count: $0) })
        let initial = try await reopened.savedReaders()
        XCTAssertEqual(initial.count, 1)
        XCTAssertEqual(initial.first?.device, first)
        XCTAssertEqual(initial.first?.lastConnected, Date(timeIntervalSince1970: 100))
        XCTAssertNil(initial.first?.lastSuccessfulSync)
        var changed = bytes
        changed.replaceSubrange(18 ..< 34, with: Data(repeating: 9, count: 16)); changed[39] = 50
        let second = try DeviceDescriptor(decoding: changed)
        try await reopened.saveReader(second, at: Date(timeIntervalSince1970: 200))
        try await reopened.saveReader(second, at: Date(timeIntervalSince1970: 200))
        let final = try await library.savedReaders()
        XCTAssertEqual(final.count, 1)
        XCTAssertEqual(final.first?.id, first.identity)
        XCTAssertEqual(final.first?.device.storageGeneration, second.storageGeneration)
        XCTAssertEqual(final.first?.device.batteryPercent, 50)
        XCTAssertNil(final.first?.lastSuccessfulSync)
    }
    func testVerifiedSyncPersistsMonotonicallyAndDoesNotCrossCards() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: url)
        let bytes = try fixture(), device = try DeviceDescriptor(decoding: bytes)
        let inventory = try ReaderInventory(reader: device.identity, generation: device.storageGeneration,
                                           contents: [], complete: true)
        let frontier = try TintaJournalFrontier.digest([])
        let checkpoint = VerifiedReaderJournalCheckpoint(reader: device.identity, generation: device.storageGeneration,
            snapshot: try JournalMergeSnapshot(count: 0, recordSize: 512, frontier: frontier), mutations: [])
        try await library.saveReader(device)
        let withoutBaseline = try await library.recordSuccessfulReaderSync(checkpoint, inventory: inventory)
        XCTAssertFalse(withoutBaseline)
        _ = try await library.importReaderJournal([], reader: device.identity, generation: device.storageGeneration,
                                                  frontier: frontier, count: 0)
        for seconds in [200.0, 100.0] {
            let recorded = try await library.recordSuccessfulReaderSync(checkpoint, inventory: inventory,
                                                                        at: Date(timeIntervalSince1970: seconds))
            XCTAssertTrue(recorded)
        }
        let reopened = try LibraryStore(url: url, random: { Data(repeating: 7, count: $0) })
        try await reopened.saveReader(device)
        let saved = try await reopened.savedReaders()
        XCTAssertEqual(saved.first?.lastSuccessfulSync, Date(timeIntervalSince1970: 200))
        for seconds in [-1, Double.infinity, Double.nan, Double(Int64.max)] {
            do {
                _ = try await reopened.recordSuccessfulReaderSync(checkpoint, inventory: inventory,
                                                                   at: Date(timeIntervalSince1970: seconds))
                XCTFail("invalid timestamp accepted")
            } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        }
        let book = LibraryContent(id: try ContentID(String(repeating: "a", count: 64)), kind: .epub,
            length: 123, title: "Selected book", originalFilename: "selected.epub")
        try await reopened.put(book)
        _ = try await reopened.setReaderSelection(reader: device.identity, content: book.id, selected: true)
        let missingContent = try await reopened.recordSuccessfulReaderSync(checkpoint, inventory: inventory,
            at: Date(timeIntervalSince1970: 250))
        XCTAssertFalse(missingContent)
        let waitingForContent = try await reopened.savedReaders()
        XCTAssertEqual(waitingForContent.first?.lastSuccessfulSync, Date(timeIntervalSince1970: 200))
        _ = try await reopened.setReaderSelection(reader: device.identity, content: book.id, selected: false)
        let deselected = try await reopened.recordSuccessfulReaderSync(checkpoint, inventory: inventory,
            at: Date(timeIntervalSince1970: 250))
        XCTAssertTrue(deselected)
        let retainedBook = try await reopened.content(book.id)
        XCTAssertEqual(retainedBook, book)
        let preference = try await reopened.appendPreference(origin: Data(repeating: 3, count: 16),
            preference: PreferenceBody(key: .fontPointSize, value: .integer(14)))
        let raced = try await reopened.recordSuccessfulReaderSync(checkpoint, inventory: inventory,
                                                                  at: Date(timeIntervalSince1970: 300))
        XCTAssertFalse(raced)
        let beforeRetry = try await reopened.savedReaders()
        XCTAssertEqual(beforeRetry.first?.lastSuccessfulSync, Date(timeIntervalSince1970: 250))
        let updatedFrontier = try TintaJournalFrontier.digest([preference])
        _ = try await reopened.importReaderJournal([preference], reader: device.identity,
            generation: device.storageGeneration, frontier: updatedFrontier, count: 1)
        let updated = VerifiedReaderJournalCheckpoint(reader: device.identity, generation: device.storageGeneration,
            snapshot: try JournalMergeSnapshot(count: 1, recordSize: 1024, frontier: updatedFrontier), mutations: [preference])
        let retried = try await reopened.recordSuccessfulReaderSync(updated, inventory: inventory,
                                                                    at: Date(timeIntervalSince1970: 300))
        XCTAssertTrue(retried)
        let afterRetry = try await reopened.savedReaders()
        XCTAssertEqual(afterRetry.first?.lastSuccessfulSync, Date(timeIntervalSince1970: 300))
        var changed = bytes
        changed.replaceSubrange(18 ..< 34, with: Data(repeating: 9, count: 16))
        try await reopened.saveReader(DeviceDescriptor(decoding: changed))
        let stale = try await reopened.recordSuccessfulReaderSync(checkpoint, inventory: inventory)
        XCTAssertFalse(stale)
        let replacement = try await library.savedReaders()
        XCTAssertNil(replacement.first?.lastSuccessfulSync)
    }
    func testFullyExportedConcurrentPreferencesStillRequireResolution() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("first.sqlite"),
            random: { Data(repeating: 7, count: $0) })
        let other = try LibraryStore(url: root.appendingPathComponent("second.sqlite"),
            random: { Data(repeating: 8, count: $0) })
        let first = try await library.appendPreference(origin: Data(repeating: 3, count: 16),
            preference: PreferenceBody(key: .fontPointSize, value: .integer(14)))
        let second = try await other.appendPreference(origin: Data(repeating: 4, count: 16),
            preference: PreferenceBody(key: .fontPointSize, value: .integer(16)))
        let device = try DeviceDescriptor(decoding: fixture())
        try await library.saveReader(device)
        let mutations = [first, second], frontier = try TintaJournalFrontier.digest(mutations)
        _ = try await library.importReaderJournal(mutations, reader: device.identity,
            generation: device.storageGeneration, frontier: frontier, count: 2)
        let checkpoint = VerifiedReaderJournalCheckpoint(reader: device.identity, generation: device.storageGeneration,
            snapshot: try JournalMergeSnapshot(count: 2, recordSize: 1024, frontier: frontier), mutations: mutations)
        let inventory = try ReaderInventory(reader: device.identity, generation: device.storageGeneration,
            contents: [], complete: true)
        let preferences = try await library.preferences()
        XCTAssertTrue(preferences.contains(where: { $0.requiresResolution }))
        let recorded = try await library.recordSuccessfulReaderSync(checkpoint, inventory: inventory)
        XCTAssertFalse(recorded)
        let saved = try await library.savedReaders()
        XCTAssertNil(saved.first?.lastSuccessfulSync)
    }
    func testRejectsInvalidTimesAndZeroIdentities() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let bytes = try fixture(), device = try DeviceDescriptor(decoding: bytes)
        for seconds in [-1, Double.infinity, Double.nan, Double(Int64.max)] {
            do { try await library.saveReader(device, at: Date(timeIntervalSince1970: seconds)); XCTFail("invalid time accepted") }
            catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        }
        for range in [2 ..< 18, 18 ..< 34] {
            var invalid = bytes; invalid.replaceSubrange(range, with: Data(count: 16))
            do { try await library.saveReader(DeviceDescriptor(decoding: invalid)); XCTFail("zero identity accepted") }
            catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        }
        let stored = try await library.savedReaders()
        XCTAssertTrue(stored.isEmpty)
    }
}

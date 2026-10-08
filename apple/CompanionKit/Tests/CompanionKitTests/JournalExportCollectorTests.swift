import Foundation
import XCTest
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

private actor ExportReader: CompanionTransport {
    let mutations: [JournalMutation]
    let frontier: Data
    let failAt: UInt32?
    let cancelAt: UInt32?
    private var requests = 0
    init(_ mutations: [JournalMutation], frontier: Data? = nil, failAt: UInt32? = nil, cancelAt: UInt32? = nil) throws {
        self.mutations = mutations
        self.frontier = try frontier ?? TintaJournalFrontier.digest(mutations)
        self.failAt = failAt
        self.cancelAt = cancelAt
    }
    func count() -> Int { requests }
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        requests += 1
        guard request.command == .exchangeChanges, request.payload.count == 40 else { throw ProtocolError.command }
        var reader = ByteReader(request.payload)
        _ = try reader.take(36)
        let index = UInt32(try reader.number(4))
        if index == failAt { throw URLError(.networkConnectionLost) }
        if index == cancelAt { withUnsafeCurrentTask { $0?.cancel() } }
        let end = Int(index) == mutations.count
        guard Int(index) <= mutations.count else { throw ProtocolError.value }
        var payload = Data([1, end ? 1 : 0, 0, 0])
        payload.appendLittleEndian(UInt64(mutations.count), count: 4)
        payload.appendLittleEndian(UInt64(end ? index : index + 1), count: 4)
        payload.append(frontier)
        let mutation = end ? nil : mutations[Int(index)]
        payload.appendLittleEndian(UInt64(mutation?.event.bytes.count ?? 0), count: 2)
        payload.appendLittleEndian(UInt64(mutation?.body.count ?? 0), count: 2)
        if let mutation { payload.append(mutation.event.bytes); payload.append(mutation.body) }
        return try ControlFrame(command: .exchangeChanges, response: true, requestID: request.requestID, payload: payload)
    }
}

final class JournalExportCollectorTests: XCTestCase, @unchecked Sendable {
    private func mutation(_ sequence: UInt64, byte: UInt8 = 42) throws -> JournalMutation {
        let body = try PreferenceBody(key: .tintaReviewCap, value: .integer(Int32(byte))).encoded
        let identity = try EventIdentity(origin: Data(repeating: 1, count: 16), epoch: 1, sequence: sequence)
        let event = try SyncEvent(identity: identity, storageGeneration: Data(repeating: 2, count: 16),
                                  kind: .preference, resource: PreferenceBody.scope, bodyHash: Data(SHA256.hash(data: body)))
        return try JournalMutation(event: event, body: body)
    }
    private func collect(_ reader: ExportReader, into library: LibraryStore, maximum: Int = 10) async throws -> JournalExportReceipt {
        try await JournalExportCollector().collect(reader: Data(repeating: 1, count: 16), generation: Data(repeating: 2, count: 16), capabilities: JournalExportPage.capability, transport: reader,
                                                  library: library, maximumEvents: maximum)
    }
    private func store() throws -> (URL, LibraryStore) {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        return (root, try LibraryStore(url: root.appendingPathComponent("library.sqlite")))
    }
    func testCompleteExportCommitsAndRetryDeduplicates() async throws {
        let (root, library) = try store(); defer { try? FileManager.default.removeItem(at: root) }
        let mutations = try [mutation(1), mutation(2)]
        let first = try await collect(ExportReader(mutations), into: library)
        XCTAssertEqual(first.count, 2); XCTAssertEqual(first.inserted, 2)
        XCTAssertEqual(first.mutations, mutations)
        XCTAssertEqual(first.frontier, try TintaJournalFrontier.digest(mutations))
        let retry = try await collect(ExportReader(mutations), into: library)
        XCTAssertEqual(retry.inserted, 0)
        let stored = try await library.syncEvents(); XCTAssertEqual(stored.count, 2)
    }
    func testInterruptedAndOversizedExportsDoNotCommit() async throws {
        let (root, library) = try store(); defer { try? FileManager.default.removeItem(at: root) }
        let mutations = try [mutation(1), mutation(2)]
        do { _ = try await collect(ExportReader(mutations, failAt: 1), into: library); XCTFail() }
        catch is URLError {}
        let reader = try ExportReader(mutations)
        do { _ = try await collect(reader, into: library, maximum: 1); XCTFail() }
        catch { XCTAssertEqual(error as? JournalExportCollectorError, .limitExceeded) }
        let requested = await reader.count(); XCTAssertEqual(requested, 1)
        let stored = try await library.syncEvents(); XCTAssertTrue(stored.isEmpty)
    }
    func testWrongFrontierAndMissingSequenceDoNotCommit() async throws {
        let (root, library) = try store(); defer { try? FileManager.default.removeItem(at: root) }
        do { _ = try await collect(ExportReader([mutation(1)], frontier: Data(repeating: 9, count: 32)), into: library); XCTFail() }
        catch { XCTAssertEqual(error as? HistoryError, .staleFrontier) }
        do { _ = try await collect(ExportReader([mutation(2)], frontier: Data(repeating: 9, count: 32)), into: library); XCTFail() }
        catch { guard case .sequenceGap = error as? HistoryError else { return XCTFail("Unexpected error: \(error)") } }
        let stored = try await library.syncEvents(); XCTAssertTrue(stored.isEmpty)
    }
    func testDuplicateAndEquivocatingExportsRollBack() async throws {
        let (root, library) = try store(); defer { try? FileManager.default.removeItem(at: root) }
        let first = try mutation(1)
        do { _ = try await collect(ExportReader([first, first]), into: library); XCTFail() }
        catch { XCTAssertEqual(error as? JournalExportCollectorError, .duplicateIdentity) }
        let readerID = Data(repeating: 1, count: 16), generation = Data(repeating: 2, count: 16)
        _ = try await library.importReaderJournal([first], reader: readerID, generation: generation,
                                                 frontier: TintaJournalFrontier.digest([first]), count: 1)
        let baseline = try await library.readerJournalBaseline(reader: readerID, generation: generation)
        do { _ = try await collect(ExportReader([mutation(1, byte: 43), mutation(2)]), into: library); XCTFail() }
        catch { XCTAssertEqual(error as? HistoryError, .equivocation(first.event.identity)) }
        let stored = try await library.syncEvents(); XCTAssertEqual(stored, [first.event])
        let after = try await library.readerJournalBaseline(reader: readerID, generation: generation)
        XCTAssertEqual(after, baseline)
    }
    func testEmptyExportAndUnsupportedReader() async throws {
        let (root, library) = try store(); defer { try? FileManager.default.removeItem(at: root) }
        let empty = try await collect(ExportReader([]), into: library, maximum: 0)
        XCTAssertEqual(empty.count, 0); XCTAssertEqual(empty.inserted, 0)
        let reader = try ExportReader([])
        do {
            _ = try await JournalExportCollector().collect(reader: Data(repeating: 1, count: 16), generation: Data(repeating: 2, count: 16), capabilities: 0, transport: reader, library: library, maximumEvents: 0)
            XCTFail()
        } catch { XCTAssertEqual(error as? ReaderSessionError, .unsupportedProtocol) }
        let requested = await reader.count(); XCTAssertEqual(requested, 0)
    }
    func testCancellationBeforeCommitLeavesLibraryUnchanged() async throws {
        let (root, library) = try store(); defer { try? FileManager.default.removeItem(at: root) }
        let first = try mutation(1)
        let reader = try ExportReader([first], cancelAt: 1)
        let collection = Task { try await self.collect(reader, into: library) }
        do { _ = try await collection.value; XCTFail() } catch is CancellationError {}
        let cancelledImport = Task {
            withUnsafeCurrentTask { $0?.cancel() }
            return try await library.importEvents([first])
        }
        do { _ = try await cancelledImport.value; XCTFail() } catch is CancellationError {}
        let stored = try await library.syncEvents(); XCTAssertTrue(stored.isEmpty)
    }
    func testHashValidMalformedBodiesAndBindingsAreRejectedBeforeImport() async throws {
        let (root, library) = try store(); defer { try? FileManager.default.removeItem(at: root) }
        let identity = try EventIdentity(origin: Data(repeating: 1, count: 16), epoch: 1, sequence: 1)
        let preference = try PreferenceBody(key: .tintaReviewCap, value: .integer(42)).encoded
        let cases: [(SyncEventKind, Data, Data, UInt32)] = [
            (.readingPosition, Data([42]), Data(repeating: 3, count: 32), 0),
            (.bookmarkPut, Data([42]), Data(repeating: 3, count: 32), 0),
            (.bookmarkDelete, Data([42]), Data(repeating: 3, count: 32), 0),
            (.preference, Data([42]), PreferenceBody.scope, 0),
            (.star, Data([42]), Data(repeating: 3, count: 32), 0),
            (.preference, preference, Data(repeating: 3, count: 32), 0),
            (.readingPosition, ReadingAnchor(spine: 1, visibleTextOffset: 2).encoded, Data(count: 32), 0),
            (.readingPosition, ReadingAnchor(spine: 1, visibleTextOffset: 2).encoded, Data(repeating: 3, count: 32), 1)
        ]
        for (kind, body, resource, scheduler) in cases {
            let event = try SyncEvent(identity: identity, storageGeneration: Data(repeating: 2, count: 16),
                                      kind: kind, resource: resource, bodyHash: Data(SHA256.hash(data: body)), schedulerVersion: scheduler)
            let mutation = try JournalMutation(event: event, body: body)
            let reader = try ExportReader([mutation], frontier: Data(repeating: 9, count: 32))
            do { _ = try await collect(reader, into: library); XCTFail("Malformed \(kind) accepted") } catch {}
            let requests = await reader.count(); XCTAssertEqual(requests, 1)
        }
        let stored = try await library.syncEvents(); XCTAssertTrue(stored.isEmpty)
    }
    func testReadingBookmarkAndPreferenceBodiesImportTogether() async throws {
        let (root, library) = try store(); defer { try? FileManager.default.removeItem(at: root) }
        let anchor = ReadingAnchor(spine: 1, visibleTextOffset: 2)
        let bookmarkID = Data(repeating: 4, count: 16)
        let put = try BookmarkBody(identity: bookmarkID, value: BookmarkValue(anchor: anchor, name: "Place", summary: "Saved"))
        let delete = try BookmarkBody(identity: bookmarkID, value: nil)
        let preference = try PreferenceBody(key: .tintaReviewCap, value: .integer(42))
        let bodies: [(SyncEventKind, Data, Data)] = [
            (.readingPosition, anchor.encoded, Data(repeating: 3, count: 32)),
            (.bookmarkPut, put.encoded, Data(repeating: 3, count: 32)),
            (.preference, preference.encoded, PreferenceBody.scope),
            (.bookmarkDelete, delete.encoded, Data(repeating: 3, count: 32))
        ]
        var mutations: [JournalMutation] = []; mutations.reserveCapacity(bodies.count)
        for (index, entry) in bodies.enumerated() {
            let identity = try EventIdentity(origin: Data(repeating: 1, count: 16), epoch: 1, sequence: UInt64(index + 1))
            let event = try SyncEvent(identity: identity, storageGeneration: Data(repeating: 2, count: 16),
                                      kind: entry.0, resource: entry.2, bodyHash: Data(SHA256.hash(data: entry.1)))
            mutations.append(try JournalMutation(event: event, body: entry.1))
        }
        let receipt = try await collect(ExportReader(mutations), into: library)
        XCTAssertEqual(receipt.inserted, 4)
        let stored = try await library.syncEvents(); XCTAssertEqual(stored.count, 4)
    }
    func testBaselinePersistsAndSeparatesReaderAndCard() async throws {
        let (root, library) = try store(); defer { try? FileManager.default.removeItem(at: root) }
        let receipt = try await collect(ExportReader([mutation(1)]), into: library)
        let reader = Data(repeating: 1, count: 16), generation = Data(repeating: 2, count: 16)
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let baseline = try await reopened.readerJournalBaseline(reader: reader, generation: generation)
        XCTAssertEqual(baseline?.frontier, receipt.frontier); XCTAssertEqual(baseline?.count, 1)
        XCTAssertEqual(baseline?.reader, reader); XCTAssertEqual(baseline?.generation, generation)
        let otherCard = Data(repeating: 3, count: 16), otherReader = Data(repeating: 4, count: 16)
        let missing = try await reopened.readerJournalBaseline(reader: reader, generation: otherCard)
        XCTAssertNil(missing)
        let emptyFrontier = try TintaJournalFrontier.digest([])
        _ = try await reopened.importReaderJournal([], reader: reader, generation: otherCard, frontier: emptyFrontier, count: 0)
        _ = try await reopened.importReaderJournal([], reader: otherReader, generation: generation, frontier: emptyFrontier, count: 0)
        let retained = try await reopened.readerJournalBaseline(reader: reader, generation: generation)
        XCTAssertEqual(retained, baseline)
        let card = try await reopened.readerJournalBaseline(reader: reader, generation: otherCard)
        XCTAssertEqual(card?.frontier, emptyFrontier); XCTAssertEqual(card?.count, 0)
        do {
            _ = try await reopened.importReaderJournal([], reader: reader, generation: generation, frontier: emptyFrontier, count: 1)
            XCTFail()
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let unchanged = try await reopened.readerJournalBaseline(reader: reader, generation: generation)
        XCTAssertEqual(unchanged, baseline)
    }
}

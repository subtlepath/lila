import Foundation
import XCTest
import CSQLite
@testable import CompanionKit

// Test instances contain no shared mutable fixture state.
final class LibraryStoreTests: XCTestCase, @unchecked Sendable {
    func testVersionOneMigrationPreservesContentAndInterruptedJob() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        var handle: OpaquePointer?
        XCTAssertEqual(sqlite3_open(url.path, &handle), SQLITE_OK)
        let hash = String(repeating: "a", count: 64)
        let job = UUID()
        let schema = """
        CREATE TABLE content(hash TEXT PRIMARY KEY,kind INTEGER,length INTEGER,title TEXT,filename TEXT);
        CREATE TABLE jobs(id TEXT PRIMARY KEY,reader BLOB,generation BLOB,installation BLOB,content TEXT,offset INTEGER,phase TEXT);
        INSERT INTO content VALUES('\(hash)',1,100,'Existing book','old.epub');
        INSERT INTO jobs VALUES('\(job.uuidString)',X'01010101010101010101010101010101',
          X'02020202020202020202020202020202',X'03030303030303030303030303030303','\(hash)',37,'paused');
        PRAGMA user_version=1;
        """
        XCTAssertEqual(sqlite3_exec(handle, schema, nil, nil, nil), SQLITE_OK)
        sqlite3_close(handle)
        let library = try LibraryStore(url: url)
        let id = try ContentID(hash)
        let old = try await library.content(id)
        XCTAssertEqual(old?.title, "Existing book")
        XCTAssertEqual(old?.authors, [])
        let resumed = try await library.job(job)
        XCTAssertEqual(resumed?.durableOffset, 37)
        XCTAssertEqual(resumed?.phase, .paused)
        try await library.put(LibraryContent(id: id, kind: .epub, length: 100, title: "Existing book", originalFilename: "old.epub",
                                             authors: ["Ana\u{0}José"], identifiers: ["urn:book"], languages: ["es", "en"]))
        let reopened = try LibraryStore(url: url)
        let updated = try await reopened.content(id)
        XCTAssertEqual(updated?.authors, ["Ana\u{0}José"])
        XCTAssertEqual(updated?.languages, ["es", "en"])
    }
    func testFailedMigrationRollsBackAddedColumns() throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".sqlite")
        defer { try? FileManager.default.removeItem(at: url) }
        var handle: OpaquePointer?
        XCTAssertEqual(sqlite3_open(url.path, &handle), SQLITE_OK)
        XCTAssertEqual(sqlite3_exec(handle, "CREATE TABLE content(hash TEXT,identifiers TEXT); PRAGMA user_version=1", nil, nil, nil), SQLITE_OK)
        sqlite3_close(handle)
        XCTAssertThrowsError(try LibraryStore(url: url))
        XCTAssertEqual(sqlite3_open(url.path, &handle), SQLITE_OK)
        defer { sqlite3_close(handle) }
        var statement: OpaquePointer?
        XCTAssertEqual(sqlite3_prepare_v2(handle, "SELECT count(*) FROM pragma_table_info('content') WHERE name='authors'", -1, &statement, nil), SQLITE_OK)
        defer { sqlite3_finalize(statement) }
        XCTAssertEqual(sqlite3_step(statement), SQLITE_ROW)
        XCTAssertEqual(sqlite3_column_int(statement, 0), 0)
    }
    private func directory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }
    private func identity(_ byte: UInt8) -> Data { Data(repeating: byte, count: 16) }
    func testRestartKeepsLibraryJobAndDurableOffset() async throws {
        let root = try directory()
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let id = try ContentID(String(repeating: "a", count: 64))
        let store = try LibraryStore(url: url)
        try await store.put(LibraryContent(id: id, kind: .epub, length: 1234, title: "Book 'quoted'", originalFilename: "book.epub"))
        let job = try await store.enqueue(content: id, reader: identity(1), storageGeneration: identity(2), installation: identity(3))
        try await store.checkpoint(job.id, offset: 777, phase: .paused)
        let reopened = try LibraryStore(url: url)
        let saved = try await reopened.job(job.id)
        XCTAssertEqual(saved?.durableOffset, 777)
        XCTAssertEqual(saved?.phase, .paused)
        let retry = try await reopened.enqueue(content: id, reader: identity(1), storageGeneration: identity(2), installation: identity(3), transaction: job.id)
        XCTAssertEqual(retry, saved)
        let content = try await reopened.content(id)
        XCTAssertEqual(content?.title, "Book 'quoted'")
    }
    func testCheckpointRequiresCompleteCommitAndKeepsTerminalJobsImmutable() async throws {
        let root = try directory()
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let id = try ContentID(String(repeating: "b", count: 64))
        try await store.put(LibraryContent(id: id, kind: .epub, length: 10, title: "Book", originalFilename: "book.epub"))
        let job = try await store.enqueue(content: id, reader: identity(1), storageGeneration: identity(2), installation: identity(3))
        do { try await store.checkpoint(job.id, offset: 11, phase: .transferring); XCTFail("Invalid offset accepted") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        do { try await store.checkpoint(job.id, offset: 10, phase: .completed); XCTFail("Uncommitted completion accepted") }
        catch { XCTAssertEqual(error as? StoreError, .invalidTransition) }
        try await store.checkpoint(job.id, offset: 9, phase: .transferring)
        try await store.checkpoint(job.id, offset: 5, phase: .paused)
        try await store.checkpoint(job.id, offset: 10, phase: .committing)
        try await store.checkpoint(job.id, offset: 10, phase: .completed)
        try await store.checkpoint(job.id, offset: 10, phase: .completed)
        do { try await store.checkpoint(job.id, offset: 0, phase: .transferring); XCTFail("Terminal job changed") }
        catch { XCTAssertEqual(error as? StoreError, .invalidTransition) }
        let pending = try await store.pendingJobs()
        XCTAssertTrue(pending.isEmpty)
    }
    func testIdentityAndImmutableContentValidation() async throws {
        let root = try directory()
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        XCTAssertThrowsError(try ContentID("../escape"))
        XCTAssertThrowsError(try ContentID(String(repeating: "A", count: 64)))
        let id = try ContentID(String(repeating: "c", count: 64))
        try await store.put(LibraryContent(id: id, kind: .epub, length: 10, title: "First", originalFilename: "first.epub"))
        do { try await store.put(LibraryContent(id: id, kind: .font, length: 10, title: "Changed", originalFilename: "other")); XCTFail("Content identity changed") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        do { _ = try await store.enqueue(content: id, reader: Data(), storageGeneration: identity(2), installation: identity(3)); XCTFail("Invalid reader accepted") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let job = try await store.enqueue(content: id, reader: identity(1), storageGeneration: identity(2), installation: identity(3))
        do { _ = try await store.enqueue(content: id, reader: identity(4), storageGeneration: identity(2), installation: identity(3), transaction: job.id); XCTFail("Transaction identity reused") }
        catch { XCTAssertEqual(error as? StoreError, .conflictingJob) }
    }
    func testFutureSchemaFailsClosedWithoutChangingVersion() async throws {
        let root = try directory()
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        var handle: OpaquePointer?
        XCTAssertEqual(sqlite3_open(url.path, &handle), SQLITE_OK)
        XCTAssertEqual(sqlite3_exec(handle, "PRAGMA user_version=99", nil, nil, nil), SQLITE_OK)
        sqlite3_close(handle)
        for _ in 0 ..< 10 {
            XCTAssertThrowsError(try LibraryStore(url: url)) { XCTAssertEqual($0 as? StoreError, .unsupportedSchema) }
        }
        XCTAssertEqual(sqlite3_open(url.path, &handle), SQLITE_OK)
        defer { sqlite3_close(handle) }
        var statement: OpaquePointer?
        XCTAssertEqual(sqlite3_prepare_v2(handle, "PRAGMA user_version", -1, &statement, nil), SQLITE_OK)
        defer { sqlite3_finalize(statement) }
        XCTAssertEqual(sqlite3_step(statement), SQLITE_ROW)
        XCTAssertEqual(sqlite3_column_int64(statement, 0), 99)
    }
    func testConcurrentEnqueueAcrossHandlesKeepsOneTransaction() async throws {
        let root = try directory()
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let first = try LibraryStore(url: url)
        let second = try LibraryStore(url: url)
        let contentID = try ContentID(String(repeating: "d", count: 64))
        try await first.put(LibraryContent(id: contentID, kind: .epub, length: 50, title: "Book", originalFilename: "book.epub"))
        let reader = identity(1), generation = identity(2), installation = identity(3), transaction = UUID()
        let jobs = try await withThrowingTaskGroup(of: TransferJob.self) { group in
            for index in 0 ..< 20 {
                let store = index.isMultiple(of: 2) ? first : second
                group.addTask {
                    try await store.enqueue(content: contentID, reader: reader, storageGeneration: generation,
                                            installation: installation, transaction: transaction)
                }
            }
            var jobs: [TransferJob] = []
            jobs.reserveCapacity(20)
            for try await job in group { jobs.append(job) }
            return jobs
        }
        XCTAssertEqual(jobs.count, 20)
        XCTAssertTrue(jobs.allSatisfy { $0.id == transaction && $0.phase == .queued })
        let pending = try await first.pendingJobs()
        XCTAssertEqual(pending.count, 1)
    }

}

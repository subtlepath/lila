import Foundation
import XCTest
import CSQLite
@testable import CompanionKit

final class TransferDeclarationStoreTests: XCTestCase, @unchecked Sendable {
    private func root() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }
    private func enqueue(_ store: LibraryStore, _ id: ContentID) async throws -> TransferJob {
        try await store.enqueue(content: id, reader: Data(repeating: 1, count: 16),
            storageGeneration: Data(repeating: 2, count: 16), installation: Data(repeating: 3, count: 16))
    }
    func testCourseDeclarationRequiresValidatedPackAndConfirmedFamilyAndSurvivesRestart() async throws {
        let directory = try root()
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent("library.sqlite")
        let store = try LibraryStore(url: url)
        let id = try ContentID(String(repeating: "a", count: 64))
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { workspace.deleteLastPathComponent() }
        let metadata = try CoursePackInspector.inspect(workspace.appendingPathComponent("test/tinta/fixtures/mini.pack"))
        let content = LibraryContent(id: id, kind: .course, length: 2000, title: "Course",
                                     originalFilename: "course.pack", languages: [metadata.locale])
        try await store.put(content)
        let job = try await enqueue(store, id)
        do {
            _ = try await store.prepareTransferDeclaration(job.id, verifiedLength: 2000)
            XCTFail("unvalidated course must not create a contract")
        } catch let error as StoreError { XCTAssertEqual(error, .missingContent) }
        try await store.putCoursePack(content, metadata: metadata)
        do {
            _ = try await store.prepareTransferDeclaration(job.id, verifiedLength: 2000)
            XCTFail("course family must be confirmed")
        } catch let error as StoreError { XCTAssertEqual(error, .missingContent) }
        let absent = try await store.retainedTransferDeclaration(job.id)
        XCTAssertNil(absent)
        let family = Data(repeating: 7, count: 16)
        _ = try await store.associateCourse(id, confirmedIdentity: family)
        let first = try await store.prepareTransferDeclaration(job.id, verifiedLength: 2000)
        XCTAssertEqual(first.manifest.logicalIdentity, family)
        XCTAssertEqual(first.manifest.formatVersion, UInt32(metadata.major))
        let restarted = try LibraryStore(url: url)
        let retained = try await restarted.retainedTransferDeclaration(job.id)
        XCTAssertEqual(retained, first)
        try await restarted.checkpoint(job.id, offset: 100, phase: .transferring)
        let repeated = try await restarted.prepareTransferDeclaration(job.id, verifiedLength: 2000)
        XCTAssertEqual(repeated, first)
        XCTAssertEqual(repeated.state.durableOffset, 0)
        var handle: OpaquePointer?
        XCTAssertEqual(sqlite3_open(url.path, &handle), SQLITE_OK)
        let database = try XCTUnwrap(handle)
        defer { sqlite3_close(database) }
        // Simulate metadata corruption outside the store's immutable-content API.
        XCTAssertEqual(sqlite3_exec(database, "UPDATE content SET length=2001", nil, nil, nil), SQLITE_OK)
        do {
            _ = try await restarted.prepareTransferDeclaration(job.id, verifiedLength: 2001)
            XCTFail("a persisted contract must not change")
        } catch let error as StoreError { XCTAssertEqual(error, .conflictingJob) }
        let unchanged = try await restarted.retainedTransferDeclaration(job.id)
        XCTAssertEqual(unchanged, first)
    }
    func testVerifiedLengthMismatchAndLegacyPartialTransferDoNotCreateDeclarations() async throws {
        let directory = try root()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = try LibraryStore(url: directory.appendingPathComponent("library.sqlite"))
        let id = try ContentID(String(repeating: "b", count: 64))
        try await store.put(LibraryContent(id: id, kind: .epub, length: 2000, title: "Book", originalFilename: "book.epub"))
        let job = try await enqueue(store, id)
        do {
            _ = try await store.prepareTransferDeclaration(job.id, verifiedLength: 1999)
            XCTFail("metadata must match the verified object")
        } catch let error as VaultError { XCTAssertEqual(error, .integrity) }
        let absent = try await store.retainedTransferDeclaration(job.id)
        XCTAssertNil(absent)
        try await store.checkpoint(job.id, offset: 100, phase: .transferring)
        do {
            _ = try await store.prepareTransferDeclaration(job.id, verifiedLength: 2000)
            XCTFail("legacy transfers cannot acquire a new contract midstream")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidTransition) }
        let stillAbsent = try await store.retainedTransferDeclaration(job.id)
        XCTAssertNil(stillAbsent)
    }
    func testFailedDeclarationWriteRollsBackAndRetryPreservesEPUBContract() async throws {
        let directory = try root()
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent("library.sqlite")
        let store = try LibraryStore(url: url)
        let id = try ContentID(String(repeating: "d", count: 64))
        try await store.put(LibraryContent(id: id, kind: .epub, length: 2000, title: "Book", originalFilename: "book.epub"))
        let job = try await enqueue(store, id)
        var handle: OpaquePointer?
        XCTAssertEqual(sqlite3_open(url.path, &handle), SQLITE_OK)
        let database = try XCTUnwrap(handle)
        defer { sqlite3_close(database) }
        XCTAssertEqual(sqlite3_exec(database, "CREATE TRIGGER fail_declaration BEFORE INSERT ON job_transfer_declarations BEGIN SELECT RAISE(ABORT,'injected failure'); END", nil, nil, nil), SQLITE_OK)
        do {
            _ = try await store.prepareTransferDeclaration(job.id, verifiedLength: 2000)
            XCTFail("failed persistence must be reported")
        } catch let error as StoreError {
            guard case .database = error else { return XCTFail("unexpected error: \(error)") }
        }
        let absent = try await store.retainedTransferDeclaration(job.id)
        XCTAssertNil(absent)
        XCTAssertEqual(sqlite3_exec(database, "DROP TRIGGER fail_declaration", nil, nil, nil), SQLITE_OK)
        let saved = try await store.prepareTransferDeclaration(job.id, verifiedLength: 2000)
        XCTAssertEqual(saved.manifest.kind, .epub)
        XCTAssertEqual(saved.manifest.logicalIdentity, Data(count: 16))
        let restarted = try LibraryStore(url: url)
        let repeated = try await restarted.prepareTransferDeclaration(job.id, verifiedLength: 2000)
        XCTAssertEqual(repeated.encoded, saved.encoded)
    }
    func testAbortAndDeletedContentCannotPrepareNewContracts() async throws {
        let directory = try root()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = try LibraryStore(url: directory.appendingPathComponent("library.sqlite"))
        let id = try ContentID(String(repeating: "c", count: 64))
        try await store.put(LibraryContent(id: id, kind: .epub, length: 2000, title: "Book", originalFilename: "book.epub"))
        let job = try await enqueue(store, id)
        try await store.requestTransferAbort(job.id)
        do {
            _ = try await store.prepareTransferDeclaration(job.id, verifiedLength: 2000)
            XCTFail("pending abort must prevent new declarations")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidTransition) }
        let absent = try await store.retainedTransferDeclaration(job.id)
        XCTAssertNil(absent)
        try await store.deleteLibraryContent(id)
        do {
            _ = try await store.prepareTransferDeclaration(job.id, verifiedLength: 2000)
            XCTFail("deleted content must not start a transfer")
        } catch let error as StoreError { XCTAssertEqual(error, .missingContent) }
    }
}

import Foundation
import XCTest
import CSQLite
@testable import CompanionKit

private actor BaselineTrafficProbe: CompanionTransport {
    private var calls = 0
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        calls += 1
        throw ProtocolError.command
    }
    func count() -> Int { calls }
}

final class CourseBaselineConfirmationTests: XCTestCase, @unchecked Sendable {
    private struct Fixture {
        let database: URL
        let library: LibraryStore
        let vault: ContentVault
        let job: TransferJob
        let review: CourseBaselineReview
    }
    private func setup(_ root: URL) async throws -> Fixture {
        var repository = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { repository.deleteLastPathComponent() }
        let review = try CourseBaselineReview(decoding: Data(contentsOf:
            repository.appendingPathComponent("protocol/fixtures/CourseBaselineReview-v1.fixture")))
        let source = repository.appendingPathComponent("test/tinta/fixtures/mini.pack")
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let object = try await vault.importFile(source)
        let metadata = try CoursePackInspector.inspect(source)
        let database = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: database)
        try await library.putCoursePack(LibraryContent(id: object.id, kind: .course, length: object.length,
            title: "Original", originalFilename: "course.pack", languages: [metadata.locale]), metadata: metadata)
        _ = try await library.associateCourse(object.id, confirmedIdentity: review.course)
        let job = try await library.enqueue(content: object.id, reader: review.reader,
            storageGeneration: review.generation, installation: Data(repeating: 9, count: 16))
        return Fixture(database: database, library: library, vault: vault, job: job, review: review)
    }
    private func changedReview(_ review: CourseBaselineReview, offset: Int) throws -> CourseBaselineReview {
        var bytes = review.encoded; bytes[offset] ^= 1
        bytes.removeLast(4)
        bytes.appendLittleEndian(UInt64(legacyCRC32(bytes)), count: 4)
        return try CourseBaselineReview(decoding: bytes)
    }
    private func sql(_ database: URL, _ statement: String) throws {
        var connection: OpaquePointer?
        guard sqlite3_open(database.path, &connection) == SQLITE_OK else { throw StoreError.invalidValue }
        defer { sqlite3_close(connection) }
        guard sqlite3_exec(connection, statement, nil, nil, nil) == SQLITE_OK else { throw StoreError.invalidValue }
    }

    func testConfirmationSurvivesRestartAndCannotReplaceFrozenReview() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root)
        let absent = try await f.library.courseBaselineConfirmation(f.job.id)
        XCTAssertNil(absent)
        let consent = try await f.library.confirmCourseBaselineImport(f.job.id, review: f.review)
        XCTAssertEqual(consent.owner, f.job.installation)
        XCTAssertEqual(consent.manifest.content, f.job.content)
        let reopened = try LibraryStore(url: f.database)
        let saved = try await reopened.courseBaselineConfirmation(f.job.id)
        XCTAssertEqual(saved, consent)
        let repeated = try await reopened.confirmCourseBaselineImport(f.job.id, review: f.review)
        XCTAssertEqual(repeated, consent)
        let changed = try changedReview(f.review, offset: 96)
        do { _ = try await reopened.confirmCourseBaselineImport(f.job.id, review: changed); XCTFail() }
        catch StoreError.conflictingJob {}
        let retained = try await reopened.courseBaselineConfirmation(f.job.id)
        XCTAssertEqual(retained, consent)
        let job = try await reopened.job(f.job.id)
        XCTAssertEqual(job, f.job)
    }

    func testForeignReaderGenerationCourseAndStartedTransferCannotConfirm() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root)
        for offset in [8, 24, 40] {
            let changed = try changedReview(f.review, offset: offset)
            do { _ = try await f.library.confirmCourseBaselineImport(f.job.id, review: changed); XCTFail() }
            catch is StoreError {}
            let absent = try await f.library.courseBaselineConfirmation(f.job.id)
            XCTAssertNil(absent)
        }
        try await f.library.checkpoint(f.job.id, offset: 1, phase: .transferring)
        try await f.library.checkpoint(f.job.id, offset: 1, phase: .paused)
        do { _ = try await f.library.confirmCourseBaselineImport(f.job.id, review: f.review); XCTFail() }
        catch StoreError.invalidTransition {}
        let absent = try await f.library.courseBaselineConfirmation(f.job.id)
        XCTAssertNil(absent)
    }

    func testOrdinaryDeclarationAtZeroOffsetCannotBecomeBaselineConsent() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root)
        let manifest = try await f.library.courseManifest(f.job.content)
        let declaration = try await f.library.prepareTransferDeclaration(f.job.id, verifiedLength: manifest.length)
        try await f.library.checkpoint(f.job.id, offset: 0, phase: .transferring)
        try await f.library.checkpoint(f.job.id, offset: 0, phase: .paused)
        do { _ = try await f.library.confirmCourseBaselineImport(f.job.id, review: f.review); XCTFail() }
        catch StoreError.invalidTransition {}
        let retained = try await f.library.retainedTransferDeclaration(f.job.id)
        XCTAssertEqual(retained, declaration)
        let consent = try await f.library.courseBaselineConfirmation(f.job.id)
        XCTAssertNil(consent)
    }

    func testBaselineAndCourseSwitchConsentAreMutuallyExclusive() async throws {
        for baselineFirst in [false, true] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let f = try await setup(root)
            let old = try ContentManifest(content: ContentID(String(repeating: "ab", count: 32)), kind: .course,
                length: 100, formatVersion: 1, logicalIdentity: Data(repeating: 7, count: 16))
            let inventory = try ReaderInventory(reader: f.job.reader, generation: f.job.storageGeneration,
                contents: [old], complete: true)
            if baselineFirst {
                _ = try await f.library.confirmCourseBaselineImport(f.job.id, review: f.review)
                do { _ = try await f.library.confirmCourseSwitch(f.job.id, inventory: inventory); XCTFail() }
                catch StoreError.invalidTransition {}
                let absent = try await f.library.courseSwitchConfirmation(f.job.id)
                XCTAssertNil(absent)
            } else {
                _ = try await f.library.confirmCourseSwitch(f.job.id, inventory: inventory)
                do { _ = try await f.library.confirmCourseBaselineImport(f.job.id, review: f.review); XCTFail() }
                catch StoreError.invalidTransition {}
                let absent = try await f.library.courseBaselineConfirmation(f.job.id)
                XCTAssertNil(absent)
            }
        }
    }

    func testVersion40MigrationPreservesJobsAndCorruptBindingIsRefused() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root)
        try sql(f.database, "DROP TABLE course_baseline_confirmations; PRAGMA user_version=40;")
        let reopened = try LibraryStore(url: f.database)
        let job = try await reopened.job(f.job.id)
        XCTAssertEqual(job, f.job)
        _ = try await reopened.confirmCourseBaselineImport(f.job.id, review: f.review)
        let changed = try changedReview(f.review, offset: 96)
        let hex = changed.encoded.map { String(format: "%02x", $0) }.joined()
        try sql(f.database, "UPDATE course_baseline_confirmations SET review=X'\(hex)';")
        do { _ = try await reopened.courseBaselineConfirmation(f.job.id); XCTFail() }
        catch StoreError.conflictingJob {}
        let retained = try await reopened.job(f.job.id)
        XCTAssertEqual(retained, f.job)
    }

    func testQueueAtomicallySelectsConfirmsAndReusesExactJobAfterRestart() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root)
        try await f.library.checkpoint(f.job.id, offset: 0, phase: .aborted)
        let queued = try await f.library.queueCourseBaselineImport(content: f.job.content,
            review: f.review, installation: f.job.installation)
        XCTAssertNotEqual(queued.id, f.job.id)
        let selected = try await f.library.isReaderContentSelected(reader: f.review.reader, content: queued.content)
        XCTAssertTrue(selected)
        let saved = try await f.library.courseBaselineConfirmation(queued.id)
        XCTAssertNotNil(saved)
        let reopened = try LibraryStore(url: f.database)
        let repeated = try await reopened.queueCourseBaselineImport(content: queued.content,
            review: f.review, installation: queued.installation)
        XCTAssertEqual(repeated, queued)
        do {
            _ = try await reopened.queueCourseBaselineImport(content: queued.content,
                review: f.review, installation: Data(repeating: 8, count: 16))
            XCTFail()
        } catch StoreError.conflictingJob {}
        let retained = try await reopened.courseBaselineConfirmation(queued.id)
        XCTAssertEqual(retained, saved)
    }

    func testFailedSelectionRollsBackNewJobAndConsentTogether() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root)
        try await f.library.checkpoint(f.job.id, offset: 0, phase: .aborted)
        try sql(f.database, "CREATE TRIGGER reject_baseline_selection BEFORE INSERT ON reader_selections BEGIN SELECT RAISE(ABORT,'injected'); END;")
        do {
            _ = try await f.library.queueCourseBaselineImport(content: f.job.content,
                review: f.review, installation: f.job.installation)
            XCTFail()
        } catch StoreError.database {}
        let jobs = try await f.library.pendingJobs()
        XCTAssertTrue(jobs.isEmpty)
        let selections = try await f.library.readerSelections(reader: f.review.reader)
        XCTAssertTrue(selections.isEmpty)
        try sql(f.database, "DROP TRIGGER reject_baseline_selection;")
        let retried = try await f.library.queueCourseBaselineImport(content: f.job.content,
            review: f.review, installation: f.job.installation)
        let saved = try await f.library.courseBaselineConfirmation(retried.id)
        XCTAssertNotNil(saved)
    }

    func testBaselineDeclarationRequiresConsentAndSurvivesRestartWithoutChangingRole() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root)
        let object = try await f.vault.verifiedObject(f.job.content)
        do {
            _ = try await f.library.prepareCourseBaselineDeclaration(f.job.id, verifiedLength: object.length)
            XCTFail("Expected missing consent refusal")
        } catch StoreError.invalidTransition { }
        let consent = try await f.library.confirmCourseBaselineImport(f.job.id, review: f.review)
        do {
            _ = try await f.library.prepareTransferDeclaration(f.job.id, verifiedLength: object.length)
            XCTFail("Expected ordinary declaration refusal")
        } catch StoreError.invalidTransition { }
        do {
            _ = try await f.library.prepareCourseBaselineDeclaration(f.job.id, verifiedLength: object.length + 1)
            XCTFail("Expected length refusal")
        } catch VaultError.integrity { }
        let declaration = try await f.library.prepareCourseBaselineDeclaration(f.job.id, verifiedLength: object.length)
        XCTAssertTrue(consent.matches(generation: f.job.storageGeneration, owner: f.job.installation,
            reviewed: f.review.hash, transfer: declaration))
        let reopened = try LibraryStore(url: f.database)
        let retained = try await reopened.prepareCourseBaselineDeclaration(f.job.id, verifiedLength: object.length)
        XCTAssertEqual(retained, declaration)
        try await reopened.checkpoint(f.job.id, offset: 1, phase: .transferring)
        try await reopened.checkpoint(f.job.id, offset: 1, phase: .paused)
        let resumed = try await reopened.prepareCourseBaselineDeclaration(f.job.id, verifiedLength: object.length)
        XCTAssertEqual(resumed, declaration)
        do {
            _ = try await reopened.prepareTransferDeclaration(f.job.id, verifiedLength: object.length)
            XCTFail("Expected durable role refusal")
        } catch StoreError.invalidTransition { }
    }

    func testDeletedOrAbortedBaselineCannotPrepareDeclaration() async throws {
        for aborted in [false, true] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: root) }
            let f = try await setup(root)
            let object = try await f.vault.verifiedObject(f.job.content)
            _ = try await f.library.confirmCourseBaselineImport(f.job.id, review: f.review)
            if aborted { try await f.library.checkpoint(f.job.id, offset: 0, phase: .aborted) }
            else { _ = try await f.library.deleteLibraryContent(f.job.content) }
            do {
                _ = try await f.library.prepareCourseBaselineDeclaration(f.job.id, verifiedLength: object.length)
                XCTFail("Expected unusable job refusal")
            } catch is StoreError { }
            let declaration = try await f.library.retainedTransferDeclaration(f.job.id)
            XCTAssertNil(declaration)
        }
    }

    func testOrdinaryRunnerCannotSendBaselineJobAsNormalCourseTransfer() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let f = try await setup(root)
        _ = try await f.library.queueCourseBaselineImport(content: f.job.content,
            review: f.review, installation: f.job.installation)
        var bytes = Data([1, 1]); bytes.append(f.job.reader); bytes.append(f.job.storageGeneration)
        bytes.append(1); bytes.appendLittleEndian(UInt64(UInt32.max), count: 4)
        bytes.append(contentsOf: [80, 1, 1]); bytes.append(Data(repeating: 0, count: 32))
        let device = try DeviceDescriptor(decoding: bytes)
        let wire = BaselineTrafficProbe()
        let runner = TransferRunner(library: f.library, vault: f.vault)
        do { _ = try await runner.run(f.job.id, device: device, transport: wire); XCTFail() }
        catch TransferRunnerError.unsupportedContent {}
        do {
            _ = try await runner.prepareDeclaration(f.job.id, device: device, installation: f.job.installation)
            XCTFail()
        } catch TransferRunnerError.unsupportedContent {}
        let calls = await wire.count()
        XCTAssertEqual(calls, 0)
        let job = try await f.library.job(f.job.id)
        XCTAssertEqual(job, f.job)
        let declared = try await f.library.retainedTransferDeclaration(f.job.id)
        XCTAssertNil(declared)
    }
}

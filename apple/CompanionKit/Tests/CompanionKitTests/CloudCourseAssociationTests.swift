import Foundation
import XCTest
@testable import CompanionKit

final class CloudCourseAssociationTests: XCTestCase, @unchecked Sendable {
    func testBindingBeforePackSurvivesRestartAndAppliesOnlyAfterValidation() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        var repo = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { repo.deleteLastPathComponent() }
        let fixture = repo.appendingPathComponent("test/tinta/fixtures/mini.pack")
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let object = try await vault.importFile(fixture)
        let url = root.appendingPathComponent("library.sqlite")
        let store = try LibraryStore(url: url)
        let association = try CloudCourseAssociation(content: object.id, identity: Data(repeating: 7, count: 16))
        try await store.acceptCloudCourseAssociation(association)
        try await store.acknowledgeCloudCourseAssociation(association, account: "first")
        let before = try await store.courseIdentity(object.id); XCTAssertNil(before)
        let beforeExport = try await store.cloudCourseAssociations(); XCTAssertTrue(beforeExport.isEmpty)
        let reopened = try LibraryStore(url: url)
        let acknowledged = try await reopened.pendingCloudCourseAssociations([association], account: "first")
        XCTAssertTrue(acknowledged.isEmpty)
        let independent = try await reopened.pendingCloudCourseAssociations([association], account: "second")
        XCTAssertEqual(independent, [association])
        let other = try CloudCourseAssociation(content: object.id, identity: Data(repeating: 8, count: 16))
        do { try await reopened.acknowledgeCloudCourseAssociation(other, account: "first"); XCTFail("Wrong family acknowledged") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        do { try await reopened.acceptCloudCourseAssociation(other); XCTFail("Family changed") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let content = LibraryContent(id: object.id, kind: .course, length: object.length,
                                     title: "Course", originalFilename: "course.pack")
        let importer = ContentImporter(vault: vault, library: reopened)
        _ = try await importer.importCloudAsset(object.url, descriptor: CloudContentDescriptor(content: content))
        let confirmed = try await reopened.courseIdentity(object.id); XCTAssertEqual(confirmed, association.identity)
        try await reopened.acceptCloudCourseAssociation(association)
        let exported = try await reopened.cloudCourseAssociations(); XCTAssertEqual(exported, [association])
        let afterCursor = try await reopened.cloudCourseAssociations(after: object.id); XCTAssertTrue(afterCursor.isEmpty)
        _ = try await reopened.deleteLibraryContent(object.id)
        let hidden = try await reopened.cloudCourseAssociations(); XCTAssertTrue(hidden.isEmpty)
        let retained = try await reopened.courseIdentity(object.id); XCTAssertEqual(retained, association.identity)
    }
    func testNonCourseAndMalformedBindingRejectWithoutCreatingAssociation() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let id = try ContentID(String(repeating: "a", count: 64))
        try await store.put(LibraryContent(id: id, kind: .epub, length: 10, title: "Book", originalFilename: "book.epub"))
        let association = try CloudCourseAssociation(content: id, identity: Data(repeating: 1, count: 16))
        do { try await store.acceptCloudCourseAssociation(association); XCTFail() }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let absent = try await store.courseIdentity(id); XCTAssertNil(absent)
        XCTAssertThrowsError(try CloudCourseAssociation(content: id, identity: Data(count: 16)))
        XCTAssertThrowsError(try CloudCourseAssociation(content: id, identity: Data([1])))
        let pendingID = try ContentID(String(repeating: "b", count: 64))
        try await store.acceptCloudCourseAssociation(CloudCourseAssociation(content: pendingID, identity: association.identity))
        do {
            try await store.put(LibraryContent(id: pendingID, kind: .epub, length: 10, title: "Book", originalFilename: "book.epub"))
            XCTFail("Pending course binding accepted non-course bytes")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let absentContent = try await store.content(pendingID); XCTAssertNil(absentContent)
    }
}

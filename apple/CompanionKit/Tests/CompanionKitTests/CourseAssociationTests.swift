import Foundation
import XCTest
import CSQLite
@testable import CompanionKit

final class CourseAssociationTests: XCTestCase, @unchecked Sendable {
    func testConfirmedAssociationSurvivesRestartAndCannotRetargetHistory() async throws {
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { workspace.deleteLastPathComponent() }
        let metadata = try CoursePackInspector.inspect(workspace.appendingPathComponent("test/tinta/fixtures/mini.pack"))
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: url)
        let first = try ContentID(String(repeating: "a", count: 64))
        let second = try ContentID(String(repeating: "b", count: 64))
        let identity = Data(repeating: 7, count: 16)
        for invalid in [Data(), Data(count: 16), Data(count: 17)] {
            do {
                _ = try await library.associateCourse(first, confirmedIdentity: invalid)
                XCTFail("invalid identities must be rejected")
            } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        }
        try await library.put(LibraryContent(id: first, kind: .course, length: 3, title: "Old import", originalFilename: "course.pack"))
        do {
            _ = try await library.associateCourse(first, confirmedIdentity: identity)
            XCTFail("unverified pack must not receive an association")
        } catch let error as StoreError { XCTAssertEqual(error, .missingContent) }
        for id in [first, second] {
            try await library.putCoursePack(LibraryContent(id: id, kind: .course, length: 3,
                title: "Course", originalFilename: "course.pack", languages: [metadata.locale]), metadata: metadata)
        }
        do {
            _ = try await library.courseManifest(first)
            XCTFail("manifest needs an explicit course association")
        } catch let error as StoreError { XCTAssertEqual(error, .missingContent) }
        let inserted = try await library.associateCourse(first, confirmedIdentity: identity)
        XCTAssertTrue(inserted)
        let repeated = try await library.associateCourse(first, confirmedIdentity: identity)
        XCTAssertFalse(repeated)
        _ = try await library.associateCourse(second, confirmedIdentity: identity)
        let foreign = try ContentID(String(repeating: "c", count: 64))
        let foreignMetadata = CoursePackMetadata(major: metadata.major, minor: metadata.minor,
            contentVersion: metadata.contentVersion, locale: "fr", itemIdentities: metadata.itemIdentities,
            recognitionItems: metadata.recognitionItems, storyIdentities: metadata.storyIdentities,
            lessonIdentities: metadata.lessonIdentities, lessonCount: metadata.lessonCount,
            legacyStoryIdentities: metadata.legacyStoryIdentities, stories: metadata.stories)
        try await library.putCoursePack(LibraryContent(id: foreign, kind: .course, length: 3,
            title: "Other language", originalFilename: "course.pack", languages: ["fr"]), metadata: foreignMetadata)
        do {
            _ = try await library.associateCourse(foreign, confirmedIdentity: identity)
            XCTFail("another language must not share the existing learner history")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let unassociated = try await library.courseIdentity(foreign)
        XCTAssertNil(unassociated)
        do {
            try await library.acceptCloudCourseAssociation(CloudCourseAssociation(content: foreign, identity: identity))
            XCTFail("cloud association must enforce the same language boundary")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let foreignInserted = try await library.associateCourse(foreign, confirmedIdentity: Data(repeating: 8, count: 16))
        XCTAssertTrue(foreignInserted)
        let casing = try ContentID(String(repeating: "d", count: 64))
        let casingMetadata = CoursePackMetadata(major: metadata.major, minor: metadata.minor,
            contentVersion: metadata.contentVersion, locale: metadata.locale.lowercased(), itemIdentities: metadata.itemIdentities,
            recognitionItems: metadata.recognitionItems, storyIdentities: metadata.storyIdentities,
            lessonIdentities: metadata.lessonIdentities, lessonCount: metadata.lessonCount,
            legacyStoryIdentities: metadata.legacyStoryIdentities, stories: metadata.stories)
        try await library.putCoursePack(LibraryContent(id: casing, kind: .course, length: 3,
            title: "Update", originalFilename: "course.pack", languages: [casingMetadata.locale]), metadata: casingMetadata)
        let casingInserted = try await library.associateCourse(casing, confirmedIdentity: identity)
        XCTAssertTrue(casingInserted)
        let pending = try ContentID(String(repeating: "e", count: 64))
        try await library.acceptCloudCourseAssociation(CloudCourseAssociation(content: pending, identity: identity))
        do {
            try await library.putCoursePack(LibraryContent(id: pending, kind: .course, length: 3,
                title: "Cloud import", originalFilename: "course.pack", languages: ["fr"]), metadata: foreignMetadata)
            XCTFail("deferred cloud identity must be checked once the pack language is known")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let rolledBack = try await library.content(pending)
        XCTAssertNil(rolledBack)
        do {
            _ = try await library.associateCourse(first, confirmedIdentity: Data(repeating: 8, count: 16))
            XCTFail("existing hash must not be moved to another course history")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let reopened = try LibraryStore(url: url)
        let manifest = try await reopened.courseManifest(first)
        XCTAssertEqual(manifest.logicalIdentity, identity)
        XCTAssertEqual(manifest.kind, .course)
        XCTAssertEqual(manifest.formatVersion, 1)
        XCTAssertEqual(try ContentManifest(decoding: manifest.encoded), manifest)
        let other = try await reopened.courseIdentity(second)
        XCTAssertEqual(other, identity)
        var connection: OpaquePointer?
        XCTAssertEqual(sqlite3_open(url.path, &connection), SQLITE_OK)
        defer { sqlite3_close(connection) }
        let identityHex = identity.map { String(format: "%02x", $0) }.joined()
        XCTAssertEqual(sqlite3_exec(connection,
            "UPDATE course_associations SET identity=X'\(identityHex)' WHERE content='\(foreign.hex)'",
            nil, nil, nil), SQLITE_OK)
        do {
            _ = try await reopened.courseManifest(foreign)
            XCTFail("legacy incompatible association must not produce a transfer manifest")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        XCTAssertEqual(sqlite3_exec(connection, "DELETE FROM course_associations WHERE content='\(foreign.hex)'",
                                   nil, nil, nil), SQLITE_OK)
        _ = try await reopened.deleteLibraryContent(first)
        do {
            _ = try await reopened.courseManifest(first)
            XCTFail("deleted course must not produce an install manifest")
        } catch let error as StoreError { XCTAssertEqual(error, .missingContent) }
        let retained = try await reopened.courseIdentity(first)
        XCTAssertEqual(retained, identity)
    }

    func testManifestEncodingMatchesSharedFixture() throws {
        let manifest = try ContentManifest(content: ContentID((0 ..< 32).map { String(format: "%02x", $0) }.joined()),
            kind: .course, length: 123456, formatVersion: 1, logicalIdentity: Data(1 ... 16))
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0 ..< 5 { workspace.deleteLastPathComponent() }
        let fixture = try JSONSerialization.jsonObject(with: Data(contentsOf:
            workspace.appendingPathComponent("protocol/fixtures/ContentManifest.json"))) as! [String: Any]
        XCTAssertEqual(manifest.encoded.map { String(format: "%02x", $0) }.joined(), fixture["binaryHex"] as? String)
        XCTAssertThrowsError(try ContentManifest(content: manifest.content, kind: .course, length: 1,
                                                formatVersion: 1, logicalIdentity: Data()))
    }
}

import Foundation
import XCTest
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

final class CourseHistoryBindingTests: XCTestCase, @unchecked Sendable {
    func testPendingCloudBindingsAndOfflineHistoryRejectDifferentFamiliesInEitherOrder() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let first = try ContentID(String(repeating: "a", count: 64))
        let second = try ContentID(String(repeating: "b", count: 64))
        try await store.acceptCloudCourseAssociation(CloudCourseAssociation(content: first, identity: Data(repeating: 7, count: 16)))
        do { _ = try await store.importTintaEvents([mutation(resource: first, course: 8, origin: 1)]); XCTFail() }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        _ = try await store.importTintaEvents([mutation(resource: first, course: 7, origin: 1)])
        _ = try await store.importTintaEvents([mutation(resource: second, course: 8, origin: 2)])
        do {
            try await store.acceptCloudCourseAssociation(CloudCourseAssociation(content: second, identity: Data(repeating: 7, count: 16)))
            XCTFail("Cloud binding replaced an offline history family")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        try await store.acceptCloudCourseAssociation(CloudCourseAssociation(content: second, identity: Data(repeating: 8, count: 16)))
        let events = try await store.syncEvents(); XCTAssertEqual(events.count, 2)
    }
    private func mutation(resource: ContentID, course: UInt8, origin: UInt8) throws -> JournalMutation {
        let body = try TintaBody(subject: TintaSubject(course: Data(repeating: course, count: 16), uid: 1),
                                 value: .star(true)).encoded
        let event = try SyncEvent(identity: EventIdentity(origin: Data(repeating: origin, count: 16), epoch: 1, sequence: 1),
                                  storageGeneration: Data(repeating: 2, count: 16), kind: .star,
                                  resource: resource.digest, bodyHash: Data(SHA256.hash(data: body)))
        return try JournalMutation(event: event, body: body)
    }
    func testOfflineClaimsSurviveRestartAndConflictingBatchRollsBack() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let store = try LibraryStore(url: url)
        let id = try ContentID(String(repeating: "a", count: 64))
        let first = try mutation(resource: id, course: 7, origin: 1)
        let conflicting = try mutation(resource: id, course: 8, origin: 2)
        do {
            _ = try await store.importEvents([first, conflicting])
            XCTFail("a batch must not give one pack two course families")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let empty = try await store.syncEvents(); XCTAssertTrue(empty.isEmpty)
        let inserted = try await store.importTintaEvents([first]); XCTAssertEqual(inserted, 1)
        let reopened = try LibraryStore(url: url)
        do {
            _ = try await reopened.importEvents([conflicting])
            XCTFail("persisted claims must constrain generic event import")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let missing = try await reopened.storedEvent(conflicting.event.identity); XCTAssertNil(missing)
        let duplicate = try await reopened.importTintaEvents([first]); XCTAssertEqual(duplicate, 0)
        let other = try ContentID(String(repeating: "b", count: 64))
        let independent = try mutation(resource: other, course: 8, origin: 3)
        let independentCount = try await reopened.importEvents([independent]); XCTAssertEqual(independentCount, 1)
        let scoped = try await reopened.replayTinta(course: Data(repeating: 7, count: 16))
        XCTAssertEqual(scoped.items.count, 1)
        XCTAssertTrue(scoped.items.keys.allSatisfy { $0.course == Data(repeating: 7, count: 16) })
        XCTAssertTrue(scoped.items.values.allSatisfy { $0.bytes[14] & 4 != 0 })
    }
    func testAssociationAndImportedHistoryCannotRetargetEachOther() async throws {
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { workspace.deleteLastPathComponent() }
        let metadata = try CoursePackInspector.inspect(workspace.appendingPathComponent("test/tinta/fixtures/mini.pack"))
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let id = try ContentID(String(repeating: "a", count: 64))
        let first = try mutation(resource: id, course: 7, origin: 1)
        _ = try await store.importEvents([first])
        try await store.putCoursePack(LibraryContent(id: id, kind: .course, length: 1, title: "Course",
                                                     originalFilename: "course.pack", languages: [metadata.locale]), metadata: metadata)
        do {
            _ = try await store.associateCourse(id, confirmedIdentity: Data(repeating: 8, count: 16))
            XCTFail("association must retain the family already claimed by history")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let missing = try await store.courseIdentity(id); XCTAssertNil(missing)
        _ = try await store.associateCourse(id, confirmedIdentity: Data(repeating: 7, count: 16))
        let conflict = try mutation(resource: id, course: 8, origin: 2)
        do {
            _ = try await store.importTintaEvents([conflict])
            XCTFail("confirmed association must constrain history import")
        } catch let error as StoreError { XCTAssertEqual(error, .invalidValue) }
        let snapshot = try await store.replayTinta()
        XCTAssertEqual(snapshot.items.count, 1)
        let retained = try await store.storedEvent(first.event.identity); XCTAssertEqual(retained, first)
    }
}

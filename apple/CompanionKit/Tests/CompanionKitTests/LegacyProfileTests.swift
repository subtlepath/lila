import Foundation
import XCTest
import CSQLite
@testable import CompanionKit

func legacyProfileFixture(retention: UInt16 = 900, version: UInt16 = 1) -> Data {
    var bytes = Data("TPRF".utf8)
    bytes.appendLittleEndian(UInt64(version), count: 2); bytes.appendLittleEndian(31, count: 2)
    for value: UInt16 in [10, 100, retention, 365, 40] { bytes.appendLittleEndian(UInt64(value), count: 2) }
    bytes.append(contentsOf: [1, 0, 0, 8]); bytes.appendLittleEndian(180, count: 2)
    bytes.append(contentsOf: [0, 4]); bytes.appendLittleEndian(UInt64(UInt16(bitPattern: -60)), count: 2)
    bytes.append(Data(count: 6)); bytes.append(contentsOf: [50, 50, 0]); bytes.append(Data(count: 2))
    bytes.appendLittleEndian(UInt64(legacyCRC32(bytes)), count: 4)
    return bytes
}
final class LegacyProfileTests: XCTestCase, @unchecked Sendable {
    func testSchedulerAndLocalClockFields() throws {
        let bytes = legacyProfileFixture()
        let profile = try LegacyProfile(bytes: bytes)
        XCTAssertEqual(profile.scheduler, try SchedulerConfiguration())
        XCTAssertEqual(profile.utcOffsetMinutes, -60); XCTAssertEqual(profile.rolloverHour, 4)
        XCTAssertEqual(profile.originalBytes, bytes); XCTAssertEqual(profile.sessionSize, 40)
    }
    func testPortablePreferenceAdoptionRollsBackAsOneBatch() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let library = try LibraryStore(url: url, random: { Data(repeating: 6, count: $0) })
        let preferences = try LegacyProfile(bytes: legacyProfileFixture()).portablePreferences()
        XCTAssertEqual(preferences.count, 9)
        XCTAssertEqual(Set(preferences.map(\.key)), Set(PreferenceKey.allCases.filter { $0.rawValue >= 32 }))
        for body in preferences { XCTAssertEqual(try PreferenceBody(decoding: body.encoded), body) }
        var handle: OpaquePointer?
        XCTAssertEqual(sqlite3_open(url.path, &handle), SQLITE_OK)
        defer { sqlite3_close(handle) }
        XCTAssertEqual(sqlite3_exec(handle, "CREATE TRIGGER reject_third BEFORE INSERT ON sync_events WHEN (SELECT count(*) FROM sync_events)=2 BEGIN SELECT RAISE(ABORT,'injected'); END", nil, nil, nil), SQLITE_OK)
        do { _ = try await library.appendPreferences(origin: Data(repeating: 1, count: 16), preferences: preferences); XCTFail("Partial adoption committed") }
        catch {}
        let failed = try await library.syncEvents(); XCTAssertTrue(failed.isEmpty)
        XCTAssertEqual(sqlite3_exec(handle, "DROP TRIGGER reject_third", nil, nil, nil), SQLITE_OK)
        let adopted = try await library.appendPreferences(origin: Data(repeating: 1, count: 16), preferences: preferences)
        XCTAssertEqual(adopted.map { $0.event.identity.sequence }, Array(UInt64(1)...UInt64(9)))
        let reopened = try LibraryStore(url: url)
        let stored = try await reopened.preferences(); XCTAssertEqual(stored.count, 9)
        do { _ = try await library.appendPreferences(origin: Data(repeating: 1, count: 16), preferences: [preferences[0], preferences[0]]); XCTFail("Duplicate keys accepted") }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
    }
    func testCompletedLessonsDoNotIncludeIndependentUnlocks() throws {
        var bytes = legacyProfileFixture()
        func profile(current: UInt16, unlocked: UInt16) throws -> LegacyProfile {
            var currentBytes = Data(); currentBytes.appendLittleEndian(UInt64(current), count: 2)
            var unlockedBytes = Data(); unlockedBytes.appendLittleEndian(UInt64(unlocked), count: 2)
            bytes.replaceSubrange(30..<32, with: currentBytes)
            bytes.replaceSubrange(32..<34, with: unlockedBytes)
            var crc = Data(); crc.appendLittleEndian(UInt64(legacyCRC32(Data(bytes[0..<39]))), count: 4)
            bytes.replaceSubrange(39..<43, with: crc)
            return try LegacyProfile(bytes: bytes)
        }
        XCTAssertEqual(try profile(current: 2, unlocked: 9).completedLessonIndices(lessonCount: 10), 0..<2)
        XCTAssertEqual(try profile(current: 10, unlocked: 9).completedLessonIndices(lessonCount: 10), 0..<10)
        XCTAssertEqual(try profile(current: 0, unlocked: 0).completedLessonIndices(lessonCount: 0), 0..<0)
        XCTAssertThrowsError(try profile(current: 11, unlocked: 9).completedLessonIndices(lessonCount: 10))
        XCTAssertThrowsError(try profile(current: 2, unlocked: 10).completedLessonIndices(lessonCount: 10))
    }
    func testCorruptionUnknownVersionAndInvalidValuesReject() throws {
        var bytes = legacyProfileFixture(); bytes[12] ^= 1
        XCTAssertThrowsError(try LegacyProfile(bytes: bytes)) { XCTAssertEqual($0 as? LegacyProfileError, .corrupt) }
        XCTAssertThrowsError(try LegacyProfile(bytes: legacyProfileFixture(version: 2))) {
            XCTAssertEqual($0 as? LegacyProfileError, .unsupported)
        }
        XCTAssertThrowsError(try LegacyProfile(bytes: legacyProfileFixture(retention: 699))) {
            XCTAssertEqual($0 as? LegacyProfileError, .invalidField(4))
        }
    }
}

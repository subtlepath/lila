import Foundation
import XCTest
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

final class TintaHistoryTests: XCTestCase, @unchecked Sendable {
    func testInstallationRestorationVerifiesEveryFileAndBindings() throws {
        let installation = try TintaHistory.derivedInstallation([], course: Data(repeating: 7, count: 16),
            metadata: installationMetadata(), studyDay: 1, storageGeneration: Data(repeating: 1, count: 16),
            snapshotIdentity: Data(repeating: 2, count: 16), packHash: Data(repeating: 3, count: 32), revision: 1)
        XCTAssertEqual(try TintaDerivedInstallation(files: installation.files, manifest: installation.manifest), installation)
        let original = installation.files
        func replacing(_ index: Int, truncate: Bool) -> TintaDerivedCourseFiles {
            var parts = [original.itemState, original.localReviews, original.completedLessons,
                         original.completedReadings, original.dayLog]
            if parts[index].isEmpty { parts[index].append(1) }
            else if truncate { parts[index].removeLast() }
            else { parts[index][0] ^= 1 }
            return TintaDerivedCourseFiles(course: original.course, studyDay: original.studyDay,
                itemState: parts[0], localReviews: parts[1], completedLessons: parts[2],
                completedReadings: parts[3], dayLog: parts[4])
        }
        for index in 0 ..< 5 {
            for truncate in [false, true] {
                XCTAssertThrowsError(try TintaDerivedInstallation(files: replacing(index, truncate: truncate),
                    manifest: installation.manifest))
            }
        }
        for range in [4 ..< 20, 116 ..< 118] {
            var wrong = installation.manifest
            wrong[range.lowerBound] ^= 1
            wrong.removeLast(4)
            wrong.appendLittleEndian(UInt64(legacyCRC32(wrong)), count: 4)
            XCTAssertThrowsError(try TintaDerivedInstallation(files: original, manifest: wrong))
        }
    }
    func testDerivedReceiptRejectsCorruptTruncatedAndInvalidRecords() throws {
        var root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        for _ in 0 ..< 4 { root.deleteLastPathComponent() }
        let bytes = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/TintaDerivedManifest-v1.fixture"))
        let receipt = try TintaDerivedReceipt(decoding: bytes)
        XCTAssertEqual(receipt.course, Data(repeating: 7, count: 16))
        XCTAssertEqual(receipt.studyDay, 42)
        XCTAssertEqual(receipt.revision, 1)
        XCTAssertEqual(receipt.files.map(\.length), [1056, 0, 16, 16, 4])
        for length in 0 ..< bytes.count { XCTAssertThrowsError(try TintaDerivedReceipt(decoding: bytes.prefix(length))) }
        for offset in bytes.indices {
            var corrupt = bytes; corrupt[offset] ^= 1
            XCTAssertThrowsError(try TintaDerivedReceipt(decoding: corrupt))
        }
        for offset in [118, 120, 128, 168, 208, 248, 288] {
            var invalid = bytes
            if offset == 120 { invalid.replaceSubrange(120 ..< 128, with: Data(count: 8)) }
            else { invalid[offset] = 1 }
            invalid.replaceSubrange(328 ..< 332, with: Data())
            invalid.appendLittleEndian(UInt64(legacyCRC32(invalid)), count: 4)
            XCTAssertThrowsError(try TintaDerivedReceipt(decoding: invalid))
        }
    }
    func testDerivedInstallationBindsFilesAndCompleteJournalTogether() throws {
        let first = try mutation(review())
        let foreign = try mutation(.lessonComplete(true), origin: 2, uid: 22,
                                   ancestors: [first.event.identity], course: 8)
        func build(_ journal: [JournalMutation]) throws -> TintaDerivedInstallation {
            try TintaHistory.derivedInstallation(journal, course: Data(repeating: 7, count: 16), metadata: installationMetadata(), studyDay: 1,
                storageGeneration: Data(repeating: 1, count: 16), snapshotIdentity: Data(repeating: 2, count: 16),
                packHash: Data(repeating: 3, count: 32), revision: 1)
        }
        let installation = try build([foreign, first, first])
        XCTAssertEqual(installation, try build([first, foreign]))
        XCTAssertEqual(installation.frontierHash, try TintaJournalFrontier.digest([first, foreign]))
        XCTAssertEqual(installation.manifest.subdata(in: 52 ..< 84), installation.frontierHash)
        XCTAssertEqual(installation.files, try TintaHistory.replay([first, foreign])
            .derivedCourseFiles(course: Data(repeating: 7, count: 16), studyDay: 1))
        let ownOnly = try build([first])
        XCTAssertEqual(installation.files, ownOnly.files)
        XCTAssertNotEqual(installation.frontierHash, ownOnly.frontierHash)
        XCTAssertNotEqual(installation.manifest, ownOnly.manifest)
        XCTAssertThrowsError(try build([foreign]))
        for (index, file) in [installation.files.itemState, installation.files.localReviews,
                             installation.files.completedLessons, installation.files.completedReadings,
                             installation.files.dayLog].enumerated() {
            var receipt = ByteReader(installation.manifest.subdata(in: (128 + index * 40) ..< (168 + index * 40)))
            XCTAssertEqual(try receipt.number(8), UInt64(file.count))
            XCTAssertEqual(try receipt.take(32), Data(SHA256.hash(data: file)))
        }
    }
    func testInstallationPreservesMixedJournalSequenceAndAncestry() throws {
        let preference = try PreferenceBody(key: .tintaNewPerDay, value: .integer(10))
        let event = try SyncEvent(identity: EventIdentity(origin: Data(repeating: 1, count: 16), epoch: 1, sequence: 1),
            storageGeneration: Data(repeating: 2, count: 16), kind: .preference,
            resource: PreferenceBody.scope, bodyHash: Data(SHA256.hash(data: preference.encoded)))
        let preceding = try JournalMutation(event: event, body: preference.encoded)
        let reviewed = try mutation(review(), sequence: 2, ancestors: [event.identity])
        let installation = try TintaHistory.derivedInstallation([reviewed, preceding],
            course: Data(repeating: 7, count: 16), metadata: installationMetadata(), studyDay: 1,
            storageGeneration: Data(repeating: 1, count: 16), snapshotIdentity: Data(repeating: 2, count: 16),
            packHash: Data(repeating: 3, count: 32), revision: 1)
        XCTAssertEqual(try LegacyItemStore(bytes: installation.files.itemState).items.count, 1)
        XCTAssertEqual(installation.frontierHash, try TintaJournalFrontier.digest([preceding, reviewed]))
        XCTAssertThrowsError(try TintaJournalFrontier.digest([reviewed]))
        let changedPreference = try PreferenceBody(key: .tintaNewPerDay, value: .integer(11))
        let changedEvent = try SyncEvent(identity: event.identity, storageGeneration: event.storageGeneration,
            kind: .preference, resource: PreferenceBody.scope, bodyHash: Data(SHA256.hash(data: changedPreference.encoded)))
        let changed = try JournalMutation(event: changedEvent, body: changedPreference.encoded)
        XCTAssertNotEqual(installation.frontierHash, try TintaJournalFrontier.digest([changed, reviewed]))
    }
    private func installationMetadata(historyCount: UInt32? = nil) -> CoursePackMetadata {
        CoursePackMetadata(major: 1, minor: 0, contentVersion: 1, locale: "es",
            itemIdentities: [1], recognitionItems: [1], storyIdentities: [33], lessonIdentities: [22],
            lessonCount: 1, legacyStoryIdentities: [44], itemHistoryCount: historyCount)
    }
    func testInstallationChecksNamespacesAndRetainsRetiredHistory() throws {
        func build(_ journal: [JournalMutation], historyCount: UInt32? = nil) throws -> TintaDerivedInstallation {
            try TintaHistory.derivedInstallation(journal, course: Data(repeating: 7, count: 16),
                metadata: installationMetadata(historyCount: historyCount), studyDay: 1,
                storageGeneration: Data(repeating: 1, count: 16), snapshotIdentity: Data(repeating: 2, count: 16),
                packHash: Data(repeating: 3, count: 32), revision: 1)
        }
        let retired = try mutation(review(), uid: 2)
        XCTAssertThrowsError(try build([retired]))
        let accepted = try build([retired], historyCount: 2)
        XCTAssertEqual(try LegacyItemStore(bytes: accepted.files.itemState).items.keys.sorted(), [2])
        XCTAssertEqual(accepted.frontierHash, try TintaJournalFrontier.digest([retired]))
        XCTAssertThrowsError(try build([mutation(review(), uid: 3)], historyCount: 2))
        XCTAssertNoThrow(try build([mutation(.lessonComplete(true), uid: 22)]))
        XCTAssertNoThrow(try build([mutation(.readingComplete(true), uid: 33)]))
        XCTAssertThrowsError(try build([mutation(.readingComplete(false), uid: 44)]))
        XCTAssertThrowsError(try build([mutation(.lessonComplete(false), uid: 33)]))
        XCTAssertThrowsError(try build([mutation(.star(true), uid: 22)]))
        let unknown = try mutation(review(), uid: 99)
        let undo = try mutation(.undo(unknown.event.identity), sequence: 2, uid: 99,
                                ancestors: [unknown.event.identity])
        XCTAssertThrowsError(try build([unknown, undo]))
    }
    func testSharedCanonicalFrontierFixture() throws {
        var root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        for _ in 0 ..< 4 { root.deleteLastPathComponent() }
        let fixture = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/TintaJournalFrontier-v1.fixture"))
        XCTAssertEqual(fixture.count, 410)
        let first = try mutation(.star(true))
        let second = try mutation(.star(false), sequence: 2, ancestors: [first.event.identity])
        var expected = Data("TJF1".utf8)
        expected.appendLittleEndian(2, count: 8)
        for event in [first.event, second.event] {
            expected.appendLittleEndian(UInt64(event.bytes.count), count: 2)
            expected.append(event.bytes)
        }
        XCTAssertEqual(expected, fixture.dropLast(32))
        XCTAssertEqual(try TintaJournalFrontier.digest([second, first, first]), fixture.suffix(32))
    }
    func testCanonicalJournalFrontierDeduplicatesAndBindsExactHistory() throws {
        let first = try mutation(review())
        let second = try mutation(.star(true), sequence: 2, ancestors: [first.event.identity])
        let digest = try TintaJournalFrontier.digest([first, second])
        XCTAssertEqual(digest, try TintaJournalFrontier.digest([second, first, second]))
        XCTAssertNotEqual(digest, try TintaJournalFrontier.digest([first]))
        let changed = try mutation(.star(false), sequence: 2, ancestors: [first.event.identity])
        XCTAssertNotEqual(digest, try TintaJournalFrontier.digest([first, changed]))
        XCTAssertThrowsError(try TintaJournalFrontier.digest([first, second, changed]))
        XCTAssertThrowsError(try TintaJournalFrontier.digest([second]))
        let undone = try mutation(.undo(first.event.identity), sequence: 3, ancestors: [first.event.identity, second.event.identity])
        XCTAssertNotEqual(digest, try TintaJournalFrontier.digest([first, second, undone]))
        let emptyPrefix = Data([84, 74, 70, 49, 0, 0, 0, 0, 0, 0, 0, 0])
        XCTAssertEqual(try TintaJournalFrontier.digest([]), Data(SHA256.hash(data: emptyPrefix)))
    }
    func testDerivedCourseFilesExcludeForeignHistoryAndApplyCompletionRemoval() throws {
        let lesson = try mutation(.lessonComplete(true), uid: 22)
        let reading = try mutation(.readingComplete(true), sequence: 2, uid: 33, ancestors: [lesson.event.identity])
        let reviewed = try mutation(review(), sequence: 3, ancestors: [reading.event.identity])
        let foreignLesson = try mutation(.lessonComplete(true), origin: 2, uid: 99,
                                         ancestors: [reviewed.event.identity], course: 8)
        let foreignReading = try mutation(.readingComplete(true), origin: 2, sequence: 2, uid: 44,
                                          ancestors: [foreignLesson.event.identity], course: 8)
        let foreignReview = try mutation(review(), origin: 2, sequence: 3, day: 2,
                                         ancestors: [foreignReading.event.identity], course: 8)
        let removed = try mutation(.readingComplete(false), sequence: 4, uid: 33,
                                  ancestors: [foreignReview.event.identity])
        let snapshot = try TintaHistory.replay([removed, foreignReview, reading, foreignReading,
                                              reviewed, lesson, foreignLesson, reviewed])
        let own = try snapshot.derivedCourseFiles(course: Data(repeating: 7, count: 16), studyDay: 1)
        XCTAssertEqual(try LegacyItemStore(bytes: own.itemState).items.count, 1)
        XCTAssertTrue(own.localReviews.isEmpty)
        let manifest = try own.installationManifest(storageGeneration: Data(repeating: 1, count: 16),
            snapshotIdentity: Data(repeating: 2, count: 16), packHash: Data(repeating: 3, count: 32),
            frontierHash: Data(repeating: 4, count: 32), revision: 1)
        XCTAssertEqual(manifest.count, 332)
        var receipt = ByteReader(manifest)
        XCTAssertEqual(try receipt.take(4), Data("TDS1".utf8))
        XCTAssertEqual(try receipt.take(16), own.course)
        _ = try receipt.take(96)
        XCTAssertEqual(try receipt.number(2), 1)
        XCTAssertEqual(try receipt.number(2), 0)
        XCTAssertEqual(try receipt.number(8), 1)
        for file in [own.itemState, own.localReviews, own.completedLessons, own.completedReadings, own.dayLog] {
            XCTAssertEqual(try receipt.number(8), UInt64(file.count))
            XCTAssertEqual(try receipt.take(32), Data(SHA256.hash(data: file)))
        }
        XCTAssertEqual(try receipt.number(4), UInt64(legacyCRC32(Data(manifest.dropLast(4)))))
        XCTAssertThrowsError(try own.installationManifest(storageGeneration: Data(count: 16),
            snapshotIdentity: Data(repeating: 2, count: 16), packHash: Data(repeating: 3, count: 32),
            frontierHash: Data(repeating: 4, count: 32), revision: 1))
        let lessons = try TintaCompletionSet(decoding: own.completedLessons)
        XCTAssertEqual(lessons.kind, .lessons); XCTAssertEqual(lessons.identities, [22])
        let readings = try TintaCompletionSet(decoding: own.completedReadings)
        XCTAssertEqual(readings.kind, .readings); XCTAssertTrue(readings.identities.isEmpty)
        XCTAssertEqual(own.dayLog, try TintaDayLog.encode(snapshot.studyTotals[own.course]!))
        let foreign = try snapshot.derivedCourseFiles(course: Data(repeating: 8, count: 16), studyDay: 2)
        XCTAssertEqual(try TintaCompletionSet(decoding: foreign.completedLessons).identities, [99])
        XCTAssertEqual(try TintaCompletionSet(decoding: foreign.completedReadings).identities, [44])
        XCTAssertNotEqual(own.dayLog, foreign.dayLog)
        let empty = try snapshot.derivedCourseFiles(course: Data(repeating: 9, count: 16), studyDay: 1)
        XCTAssertTrue(try TintaCompletionSet(decoding: empty.completedLessons).identities.isEmpty)
        XCTAssertEqual(empty.dayLog, Data("TDL1".utf8))
        XCTAssertThrowsError(try snapshot.derivedCourseFiles(course: Data(count: 16), studyDay: 1))
        XCTAssertThrowsError(try snapshot.derivedCourseFiles(course: Data(count: 15), studyDay: 1))
    }
    private func subject(_ uid: UInt32 = 1) throws -> TintaSubject { try TintaSubject(course: Data(repeating: 7, count: 16), uid: uid) }
    private func review(_ grade: UInt8 = 3) throws -> TintaValue {
        .review(grade: grade, format: 0, responseMilliseconds: 1000, configuration: try SchedulerConfiguration())
    }
    private func mutation(_ value: TintaValue, origin: UInt8 = 1, sequence: UInt64 = 1, day: UInt32 = 1,
                          uid: UInt32 = 1, ancestors: [EventIdentity] = [], configurationHash: Data? = nil,
                          course: UInt8 = 7, resource: UInt8 = 3) throws -> JournalMutation {
        let body = try TintaBody(subject: TintaSubject(course: Data(repeating: course, count: 16), uid: uid), value: value)
        var bytes = Data([1, 3]) + Data(repeating: origin, count: 16)
        bytes.appendLittleEndian(1, count: 8); bytes.appendLittleEndian(sequence, count: 8)
        bytes.append(Data(repeating: 2, count: 16)); bytes.append(value.kind.rawValue)
        bytes.append(Data(repeating: resource, count: 32)); bytes.append(Data(SHA256.hash(data: body.encoded)))
        bytes.appendLittleEndian(UInt64(day), count: 4); bytes.appendLittleEndian(0, count: 8); bytes.append(0)
        if case let .review(_, _, _, configuration) = value {
            bytes.appendLittleEndian(1, count: 4); bytes.append(configurationHash ?? Data(SHA256.hash(data: configuration.encoded)))
        } else { bytes.appendLittleEndian(0, count: 4); bytes.append(Data(count: 32)) }
        bytes.append(UInt8(ancestors.count)); for ancestor in ancestors { bytes.append(ancestor.storageKey) }
        return try JournalMutation(event: SyncEvent(decoding: bytes), body: body.encoded)
    }
    func testDailyTotalsRetainExactTimeAndExcludeUndoneDuplicateReviews() throws {
        let configuration = try SchedulerConfiguration()
        let first = try mutation(.review(grade: 1, format: 0, responseMilliseconds: 1100, configuration: configuration))
        let second = try mutation(.review(grade: 3, format: 0, responseMilliseconds: UInt32.max, configuration: configuration),
            origin: 2, ancestors: [first.event.identity])
        let third = try mutation(.review(grade: 4, format: 0, responseMilliseconds: 0, configuration: configuration),
            sequence: 2, ancestors: [first.event.identity, second.event.identity])
        let course = Data(repeating: 7, count: 16)
        let initial = try TintaHistory.replay([third, first, second, second]).studyTotals[course]?[1]
        XCTAssertEqual(initial?.gradedReviews, 3)
        XCTAssertEqual(initial?.correctReviews, 2)
        XCTAssertEqual(initial?.newItems, 1)
        XCTAssertEqual(initial?.reviews, 0)
        XCTAssertEqual(initial?.responseMilliseconds, UInt64(UInt32.max) + 1100)
        let undo = try mutation(.undo(second.event.identity), sequence: 3,
            ancestors: [second.event.identity, third.event.identity])
        let result = try TintaHistory.replay([undo, third, second, first, undo])
        XCTAssertEqual(result.studyTotals[course]?[1]?.gradedReviews, 2)
        XCTAssertEqual(result.studyTotals[course]?[1]?.correctReviews, 1)
        XCTAssertEqual(result.studyTotals[course]?[1]?.responseMilliseconds, 1100)
    }
    func testReaderDayTotalsUseGradesAndRoundAggregateTimeWithCheckedBounds() throws {
        var totals = StudyTotals()
        totals.reviews = 1
        totals.gradedReviews = 3
        totals.correctReviews = 2
        totals.newItems = 1
        for (milliseconds, seconds): (UInt64, UInt32) in [(0, 0), (499, 0), (500, 1), (1499, 1), (1500, 2)] {
            totals.responseMilliseconds = milliseconds
            let day = try totals.readerDayTotals()
            XCTAssertEqual(day.reviews, 3)
            XCTAssertEqual(day.correct, 2)
            XCTAssertEqual(day.newItems, 1)
            XCTAssertEqual(day.seconds, seconds)
        }
        totals.responseMilliseconds = UInt64(UInt32.max) * 1000 + 499
        XCTAssertEqual(try totals.readerDayTotals().seconds, UInt32.max)
        totals.responseMilliseconds += 1
        XCTAssertThrowsError(try totals.readerDayTotals())
        totals.responseMilliseconds = UInt64.max
        XCTAssertThrowsError(try totals.readerDayTotals())
        totals.responseMilliseconds = 0
        totals.correctReviews = 4
        XCTAssertThrowsError(try totals.readerDayTotals())
        totals.correctReviews = 2
        totals.newItems = 4
        XCTAssertThrowsError(try totals.readerDayTotals())
    }
    func testScopedReplayPreservesCrossCourseAncestryAndMergesEditions() throws {
        let foreign = try mutation(review(4), course: 8, resource: 4)
        let oldEdition = try mutation(review(3), sequence: 2, ancestors: [foreign.event.identity])
        let newEdition = try mutation(review(4), origin: 2, day: 2,
                                      ancestors: [oldEdition.event.identity], resource: 5)
        let undo = try mutation(.undo(oldEdition.event.identity), sequence: 3, day: 2,
                                ancestors: [oldEdition.event.identity, newEdition.event.identity], resource: 5)
        let events = [undo, newEdition, foreign, oldEdition, newEdition]
        let course = Data(repeating: 7, count: 16)
        let snapshot = try TintaHistory.replay(events, course: course)
        XCTAssertEqual(snapshot.items, [try subject(): try ScheduledItem(uid: 1).reviewed(
            grade: 4, day: 2, configuration: SchedulerConfiguration())])
        XCTAssertEqual(snapshot.undoneReviews, [oldEdition.event.identity])
        XCTAssertEqual(snapshot.studyTotals.keys.sorted(by: { $0.lexicographicallyPrecedes($1) }), [course])
        XCTAssertEqual(snapshot.studyTotals[course]?[2]?.newItems, 1)
        let other = try TintaHistory.replay(events, course: Data(repeating: 8, count: 16))
        XCTAssertEqual(other.items.count, 1)
        XCTAssertTrue(other.undoneReviews.isEmpty)
        XCTAssertEqual(try TintaHistory.replay(events).items.count, 2)
        for invalid in [Data(), Data(count: 16), Data(count: 17)] {
            XCTAssertThrowsError(try TintaHistory.replay(events, course: invalid))
        }
    }
    func testUndoAfterConcurrentReviewRetainsOtherReadersReview() throws {
        let local = try mutation(review(4)), other = try mutation(review(3), origin: 2)
        let undo = try mutation(.undo(local.event.identity), sequence: 2, day: 2, ancestors: [local.event.identity])
        let snapshot = try TintaHistory.replay([other, undo, local, other])
        let expected = try ScheduledItem(uid: 1).reviewed(grade: 3, day: 1, configuration: SchedulerConfiguration())
        XCTAssertEqual(snapshot.items[try subject()], expected)
        XCTAssertEqual(snapshot.undoneReviews, [local.event.identity])
        XCTAssertEqual(snapshot.studyTotals[Data(repeating: 7, count: 16)]?[1]?.newItems, 1)
        XCTAssertEqual(try TintaHistory.replay([local, other, undo]), snapshot)
    }
    func testControlMutationsAndTotalsReplayIdempotently() throws {
        let first = try mutation(review(4))
        let star = try mutation(.star(true), sequence: 2)
        let suspend = try mutation(.suspension(true), sequence: 3)
        let second = try mutation(review(3), sequence: 4, day: 10)
        let unsuspend = try mutation(.suspension(false), sequence: 5, day: 10)
        let lesson = try mutation(.lessonComplete(true), sequence: 6, day: 10, uid: 22)
        let reading = try mutation(.readingComplete(true), sequence: 7, day: 10, uid: 33)
        let entries = [first, star, suspend, second, unsuspend, lesson, reading]
        let snapshot = try TintaHistory.replay(entries.reversed())
        XCTAssertEqual(snapshot.items[try subject()]?.bytes[14], 4)
        XCTAssertEqual(snapshot.completedLessons, [try subject(22)])
        XCTAssertEqual(snapshot.completedReadings, [try subject(33)])
        XCTAssertEqual(snapshot.studyTotals[Data(repeating: 7, count: 16)]?[10]?.reviews, 1)
        XCTAssertEqual(try TintaHistory.replay(entries + entries), snapshot)
    }
    func testUndoRequiresMatchingCausalReviewAndConfiguration() throws {
        let first = try mutation(review(), uid: 2)
        let wrongItem = try mutation(.undo(first.event.identity), sequence: 2, ancestors: [first.event.identity])
        XCTAssertThrowsError(try TintaHistory.replay([first, wrongItem])) {
            XCTAssertEqual($0 as? TintaReplayError, .invalidUndo(first.event.identity))
        }
        XCTAssertThrowsError(try TintaBody(mutation: mutation(.undo(first.event.identity), sequence: 2)))
        XCTAssertThrowsError(try TintaBody(mutation: mutation(review(), configurationHash: Data(count: 32))))
        XCTAssertThrowsError(try TintaBody(mutation: mutation(review(), day: 65536)))
    }
    func testBodyRoundTripsAndRejectsMalformedData() throws {
        let identity = try EventIdentity(origin: Data(repeating: 1, count: 16), epoch: 1, sequence: 1)
        for value in [try review(), .undo(identity), .suspension(true), .star(false), .lessonComplete(true), .readingComplete(false)] {
            let body = try TintaBody(subject: subject(), value: value)
            XCTAssertEqual(try TintaBody(decoding: body.encoded), body)
            XCTAssertThrowsError(try TintaBody(decoding: body.encoded + Data([0])))
            XCTAssertThrowsError(try TintaBody(decoding: body.encoded.dropLast()))
        }
        XCTAssertThrowsError(try TintaBody(subject: subject(), value: review(0)))
        XCTAssertThrowsError(try TintaSubject(course: Data(count: 16), uid: 1))
        var invalid = try TintaBody(subject: subject(), value: .star(true)).encoded; invalid[22] = 2
        XCTAssertThrowsError(try TintaBody(decoding: invalid))
    }
    func testSQLiteReopenReplaysSameSnapshotAndRejectsInvalidBodiesBeforeWrite() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let store = try LibraryStore(url: url)
        let first = try mutation(review(4))
        let second = try mutation(review(3), origin: 2)
        let undo = try mutation(.undo(first.event.identity), sequence: 2, ancestors: [first.event.identity])
        try await store.importTintaEvents([undo, second, first])
        let reopened = try LibraryStore(url: url)
        let replayed = try await reopened.replayTinta()
        XCTAssertEqual(replayed, try TintaHistory.replay([first, second, undo]))
        let invalid = try mutation(review(), origin: 3, configurationHash: Data(count: 32))
        do { try await reopened.importTintaEvents([invalid]); XCTFail() } catch { XCTAssertEqual(error as? ProtocolError, .value) }
        let missing = try await reopened.storedEvent(invalid.event.identity); XCTAssertNil(missing)
    }

    func testSharedFirmwareBodyFixtures() throws {
        var root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        for _ in 0 ..< 4 { root.deleteLastPathComponent() }
        for name in ["TintaReviewBody", "TintaUndoBody"] {
            let data = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/\(name).json"))
            let object = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
            let hex = Array(try XCTUnwrap(object["binaryHex"] as? String))
            var bytes = Data(); bytes.reserveCapacity(hex.count / 2)
            for index in stride(from: 0, to: hex.count, by: 2) {
                bytes.append(try XCTUnwrap(UInt8(String(hex[index ... index + 1]), radix: 16)))
            }
            let decoded = try TintaBody(decoding: bytes)
            XCTAssertEqual(decoded.encoded, bytes)
            XCTAssertEqual(decoded.subject, try subject())
            if name == "TintaReviewBody" { XCTAssertEqual(decoded.value, try review()) }
            else {
                XCTAssertEqual(decoded.value, .undo(try EventIdentity(origin: Data(repeating: 1, count: 16), epoch: 1, sequence: 1)))
            }
        }
    }

}

import Foundation
import XCTest
@testable import CompanionKit

final class TintaSchedulerTests: XCTestCase {
    func testFlagBridgePreservesScheduleAndDerivedLeechState() throws {
        var bytes = try ScheduledItem(uid: 17).reviewed(grade: 3, day: 10, configuration: SchedulerConfiguration()).bytes
        bytes[14] = 2
        let original = try ScheduledItem(decoding: bytes)
        let suspended = try original.settingFlag(.suspension, enabled: true)
        let starred = try suspended.settingFlag(.star, enabled: true)
        XCTAssertEqual(starred.bytes[14], 7)
        let unstarred = try starred.settingFlag(.star, enabled: false)
        let restored = try unstarred.settingFlag(.suspension, enabled: false)
        XCTAssertEqual(restored, original)
        XCTAssertEqual(starred.bytes.prefix(14), original.bytes.prefix(14))
        XCTAssertEqual(original.bytes[14], 2)
    }
    func testFirmwarePackedFirstReviewStates() throws {
        let fresh = try ScheduledItem(uid: 0x01020304)
        XCTAssertEqual(Array(fresh.bytes), [4, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
        let configuration = try SchedulerConfiguration()
        let good = try fresh.reviewed(grade: 3, day: 10, configuration: configuration)
        XCTAssertEqual(Array(good.bytes), [4, 3, 2, 1, 10, 0, 10, 0, 37, 0, 53, 1, 1, 0, 0, 0])
        let easy = try fresh.reviewed(grade: 4, day: 10, configuration: configuration)
        XCTAssertEqual(Array(easy.bytes), [4, 3, 2, 1, 18, 0, 10, 0, 133, 0, 25, 2, 1, 0, 0, 0])
    }
    func testLearningLapseAndDaySaturation() throws {
        let configuration = try SchedulerConfiguration()
        let fresh = try ScheduledItem(uid: 1)
        let first = try fresh.reviewed(grade: 3, day: 10, configuration: configuration)
        let second = try first.reviewed(grade: 3, day: 10, configuration: configuration)
        XCTAssertEqual(second.bytes[11], 5)
        let third = try second.reviewed(grade: 3, day: 10, configuration: configuration)
        XCTAssertEqual(third.bytes[11], 2)
        let lapsed = try third.reviewed(grade: 1, day: 20, configuration: configuration)
        XCTAssertEqual(lapsed.bytes[11], 3); XCTAssertEqual(lapsed.bytes[13], 1)
        let saturated = try fresh.reviewed(grade: 4, day: UInt16.max, configuration: configuration)
        XCTAssertEqual(Array(saturated.bytes[4 ... 5]), [255, 255])
        let limited = try fresh.reviewed(grade: 4, day: 10, configuration: SchedulerConfiguration(maximumInterval: 2))
        XCTAssertEqual(limited.bytes[4], 12)
    }
    func testRejectsMalformedStatesGradesAndConfiguration() throws {
        XCTAssertThrowsError(try ScheduledItem(uid: 0)); XCTAssertThrowsError(try ScheduledItem(uid: UInt32.max))
        XCTAssertThrowsError(try ScheduledItem(decoding: Data(count: 15)))
        let fresh = try ScheduledItem(uid: 1), configuration = try SchedulerConfiguration()
        for grade: UInt8 in [0, 5, 255] { XCTAssertThrowsError(try fresh.reviewed(grade: grade, day: 1, configuration: configuration)) }
        var corrupt = fresh.bytes; corrupt[11] = 255
        XCTAssertThrowsError(try ScheduledItem(decoding: corrupt))
        XCTAssertThrowsError(try SchedulerConfiguration(retentionBasisPoints: 0))
        XCTAssertThrowsError(try SchedulerConfiguration(retentionBasisPoints: 10000))
        XCTAssertThrowsError(try SchedulerConfiguration(maximumInterval: 0))
        XCTAssertEqual(Array(configuration.encoded), [1, 1, 40, 35, 109, 1])
    }
}

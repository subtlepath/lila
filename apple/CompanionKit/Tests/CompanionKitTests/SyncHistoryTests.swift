import Foundation
import XCTest
@testable import CompanionKit

final class SyncHistoryTests: XCTestCase {
    private func event(origin: UInt8, sequence: UInt64 = 1, day: UInt32 = 1, time: UInt64 = 0,
                       quality: UInt8 = 0, ancestors: [(UInt8, UInt64)] = [], body: UInt8 = 4) throws -> SyncEvent {
        var bytes = Data([1, 3]); bytes.append(Data(repeating: origin, count: 16))
        bytes.appendLittleEndian(1, count: 8); bytes.appendLittleEndian(sequence, count: 8)
        bytes.append(Data(repeating: 2, count: 16)); bytes.append(5)
        bytes.append(Data(repeating: 3, count: 32)); bytes.append(Data(repeating: body, count: 32))
        bytes.appendLittleEndian(UInt64(day), count: 4); bytes.appendLittleEndian(time, count: 8)
        bytes.append(quality); bytes.appendLittleEndian(1, count: 4); bytes.append(Data(repeating: 5, count: 32))
        bytes.append(UInt8(ancestors.count))
        for (origin, sequence) in ancestors {
            bytes.append(Data(repeating: origin, count: 16)); bytes.appendLittleEndian(1, count: 8); bytes.appendLittleEndian(sequence, count: 8)
        }
        return try SyncEvent(decoding: bytes)
    }
    func testDuplicateAndReversedDeliveryProduceSameCausalOrder() throws {
        let first = try event(origin: 1, day: 9)
        let next = try event(origin: 1, sequence: 2, day: 1)
        let other = try event(origin: 2, day: 0, ancestors: [(1, 2)])
        XCTAssertEqual(try SyncHistory.merged([other, next, first, next]), [first, next, other])
        XCTAssertEqual(try SyncHistory.merged([first, next, other]), [first, next, other])
    }
    func testUntrustedClockDoesNotChangeOrdering() throws {
        let first = try event(origin: 1, time: UInt64.max, quality: 1)
        let second = try event(origin: 2, time: 1, quality: 1)
        XCTAssertEqual(try SyncHistory.merged([second, first]), [first, second])
        let early = try event(origin: 3, time: 10, quality: 2)
        let late = try event(origin: 4, time: 20, quality: 2)
        XCTAssertEqual(try SyncHistory.merged([late, early]), [early, late])
    }
    func testEquivocationMissingHistoryAndCyclesRejectReplay() throws {
        let first = try event(origin: 1)
        XCTAssertThrowsError(try SyncHistory.merged([first, event(origin: 1, body: 9)])) {
            XCTAssertEqual($0 as? HistoryError, .equivocation(first.identity))
        }
        XCTAssertThrowsError(try SyncHistory.merged([event(origin: 1, sequence: 2)]))
        XCTAssertThrowsError(try SyncHistory.merged([event(origin: 2, ancestors: [(1, 1)])]))
        XCTAssertThrowsError(try SyncHistory.merged([event(origin: 1, ancestors: [(2, 1)]), event(origin: 2, ancestors: [(1, 1)])])) {
            XCTAssertEqual($0 as? HistoryError, .causalCycle)
        }
    }
    func testMalformedIdentityAndAncestryAreRejected() throws {
        XCTAssertThrowsError(try event(origin: 0))
        XCTAssertThrowsError(try event(origin: 1, sequence: 0))
        XCTAssertThrowsError(try event(origin: 1, ancestors: [(1, 1)]))
        XCTAssertThrowsError(try event(origin: 1, ancestors: [(2, 1), (2, 1)]))
    }
}

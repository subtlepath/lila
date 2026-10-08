import Foundation
import XCTest
@testable import CompanionKit

final class TintaDayLogTests: XCTestCase {
    func testReaderLayoutAndIndependentCRCFixture() throws {
        var totals = StudyTotals()
        totals.gradedReviews = 3; totals.correctReviews = 2; totals.newItems = 1
        totals.responseMilliseconds = 1500
        XCTAssertEqual(try TintaDayLog.encode([2: totals]),
                       Data([0x54, 0x44, 0x4c, 0x31, 2, 0, 3, 0, 2, 0, 1, 0, 2, 0, 0x44, 0x1b]))
        XCTAssertEqual(try TintaDayLog.encode([:]), Data("TDL1".utf8))
    }
    func testSplittingPreservesTotalsAndSortsDays() throws {
        var totals = StudyTotals()
        totals.gradedReviews = 65536; totals.correctReviews = 65535; totals.newItems = 1
        totals.responseMilliseconds = 65536500
        let bytes = try TintaDayLog.encode([9: totals, 2: StudyTotals()])
        XCTAssertEqual(bytes.count, 40)
        var reader = ByteReader(Data(bytes.dropFirst(4)))
        var sums = [UInt64](repeating: 0, count: 4)
        for expectedDay: UInt64 in [2, 9, 9] {
            let record = try reader.take(12)
            var fields = ByteReader(record)
            XCTAssertEqual(try fields.number(2), expectedDay)
            for index in sums.indices { sums[index] += try fields.number(2) }
            XCTAssertEqual(try fields.number(2), UInt64(legacyCRC32(Data(record.prefix(10))) & 0xffff))
        }
        XCTAssertEqual(sums, [65536, 65535, 1, 65537])
    }
    func testUnrepresentableTimeIsRejected() {
        var totals = StudyTotals(); totals.responseMilliseconds = UInt64.max
        XCTAssertThrowsError(try TintaDayLog.encode([1: totals]))
    }
}

import Foundation
import XCTest
@testable import CompanionKit

final class TintaCompletionSetTests: XCTestCase {
    func testIndependentFixtureAndSetsBeyondLegacyCapacity() throws {
        let set = try TintaCompletionSet(kind: .readings, identities: [9, 7])
        XCTAssertEqual(set.encoded, Data([84, 67, 83, 49, 2, 0, 0, 0, 2, 0, 0, 0,
                                         7, 0, 0, 0, 9, 0, 0, 0, 215, 125, 242, 166]))
        for kind in [TintaCompletionSet.Kind.lessons, .readings] {
            for count in [0, 97, 65535] {
                let identities = Set((0..<count).map { UInt32($0 + 1) })
                let original = try TintaCompletionSet(kind: kind, identities: identities)
                XCTAssertEqual(try TintaCompletionSet(decoding: original.encoded), original)
            }
        }
    }
    func testCorruptTruncatedAndInvalidIdentitiesAreRejected() throws {
        let bytes = try TintaCompletionSet(kind: .readings, identities: [7, 9]).encoded
        for index in bytes.indices {
            var corrupt = bytes; corrupt[index] ^= 1
            XCTAssertThrowsError(try TintaCompletionSet(decoding: corrupt))
        }
        for length in 0..<bytes.count {
            XCTAssertThrowsError(try TintaCompletionSet(decoding: Data(bytes.prefix(length))))
        }
        XCTAssertThrowsError(try TintaCompletionSet(kind: .lessons, identities: [0]))
        XCTAssertThrowsError(try TintaCompletionSet(kind: .readings, identities: [UInt32.max]))
        XCTAssertThrowsError(try TintaCompletionSet(kind: .readings, identities: Set(UInt32(1)...UInt32(65536))))
    }
}

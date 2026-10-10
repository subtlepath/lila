import Foundation
import XCTest
@testable import CompanionKit

final class UnboundCourseMigrationRequestTests: XCTestCase {
    private func fixture(_ name: String) throws -> Data {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        return try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/" + name))
    }
    func testSharedConsentRoundTripsAndCannotDecodeAsArchiveConsent() throws {
        let bytes = try fixture("UnboundCourseMigrationRequest-v1.fixture")
        let request = try UnboundCourseMigrationRequest(decoding: bytes)
        XCTAssertEqual(request.encoded, bytes)
        XCTAssertThrowsError(try CourseBaselineImportRequest(decoding: bytes))
        let archive = try fixture("CourseBaselineImportRequest-v1.fixture")
        XCTAssertThrowsError(try UnboundCourseMigrationRequest(decoding: archive))
        let review = try CourseBaselineReview(decoding: fixture("CourseBaselineReview-unbound-v2.fixture"))
        let created = try UnboundCourseMigrationRequest(generation: request.generation, owner: request.owner,
            transaction: request.transaction, originalPack: request.originalPack, review: review)
        XCTAssertEqual(created, request)
        let state = try TransferState(transaction: request.transaction, owner: request.owner,
            storageGeneration: request.generation, contentHash: request.originalPack.content.digest,
            length: request.originalPack.length)
        let declaration = try TransferDeclaration(manifest: request.originalPack, state: state)
        XCTAssertTrue(request.matches(review: review, reader: review.reader, generation: request.generation,
            owner: request.owner, transfer: declaration))
        for foreign in [Data(repeating: 9, count: 16), Data()] {
            XCTAssertFalse(request.matches(review: review, reader: foreign, generation: request.generation,
                owner: request.owner, transfer: declaration))
            XCTAssertFalse(request.matches(review: review, reader: review.reader, generation: foreign,
                owner: request.owner, transfer: declaration))
            XCTAssertFalse(request.matches(review: review, reader: review.reader, generation: request.generation,
                owner: foreign, transfer: declaration))
        }
        let isolated = try CourseBaselineReview(decoding: fixture("CourseBaselineReview-v1.fixture"))
        XCTAssertThrowsError(try UnboundCourseMigrationRequest(generation: request.generation, owner: request.owner,
            transaction: request.transaction, originalPack: request.originalPack, review: isolated))
        XCTAssertFalse(request.matches(review: isolated, reader: isolated.reader, generation: request.generation,
            owner: request.owner, transfer: declaration))
    }
    func testEveryTruncationAndByteCorruptionRefuses() throws {
        let bytes = try fixture("UnboundCourseMigrationRequest-v1.fixture")
        for size in 0..<bytes.count {
            XCTAssertThrowsError(try UnboundCourseMigrationRequest(decoding: Data(bytes.prefix(size))))
        }
        for index in bytes.indices {
            var corrupt = bytes; corrupt[index] ^= 1
            XCTAssertThrowsError(try UnboundCourseMigrationRequest(decoding: corrupt))
        }
    }
}

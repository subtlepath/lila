import Foundation
import XCTest
@testable import CompanionKit

final class CourseBaselineImportRequestTests: XCTestCase {
    private func fixture() throws -> Data {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        return try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/CourseBaselineImportRequest-v1.fixture"))
    }

    func testSharedNativeFixtureAndEveryTruncatedOrCorruptByte() throws {
        let bytes = try fixture()
        let request = try CourseBaselineImportRequest(decoding: bytes)
        XCTAssertEqual(request.encoded, bytes)
        XCTAssertEqual(request.generation, Data(repeating: 1, count: 16))
        XCTAssertEqual(request.owner, Data(repeating: 2, count: 16))
        XCTAssertEqual(request.transaction, Data(repeating: 3, count: 16))
        XCTAssertEqual(request.reviewHash, Data(repeating: 6, count: 32))
        for length in 0..<bytes.count {
            XCTAssertThrowsError(try CourseBaselineImportRequest(decoding: Data(bytes.prefix(length))))
        }
        for offset in bytes.indices {
            var invalid = bytes; invalid[offset] ^= 1
            XCTAssertThrowsError(try CourseBaselineImportRequest(decoding: invalid))
        }
        XCTAssertThrowsError(try CourseBaselineImportRequest(decoding: bytes + Data([0])))
    }

    func testResignedEmptyIdentityAndUnsupportedManifestCannotAuthorize() throws {
        let bytes = try fixture()
        for range in [8..<24, 24..<40, 40..<56, 58..<90, 103..<119, 119..<151] {
            var invalid = bytes
            invalid.replaceSubrange(range, with: Data(repeating: 0, count: range.count))
            invalid.replaceSubrange(151..<155, with: Data())
            invalid.appendLittleEndian(UInt64(legacyCRC32(invalid)), count: 4)
            XCTAssertThrowsError(try CourseBaselineImportRequest(decoding: invalid))
        }
        let request = try CourseBaselineImportRequest(decoding: bytes)
        for length in [UInt64(0), UInt64(UInt32.max) + 1] {
            let manifest = try ContentManifest(content: request.manifest.content, kind: .course,
                length: length, formatVersion: 1, logicalIdentity: request.manifest.logicalIdentity)
            XCTAssertThrowsError(try CourseBaselineImportRequest(generation: request.generation,
                owner: request.owner, transaction: request.transaction, manifest: manifest, reviewHash: request.reviewHash))
        }
    }

    func testConsentCannotBeRetargetedToAnotherOwnerReviewOrTransaction() throws {
        let request = try CourseBaselineImportRequest(decoding: fixture())
        func transfer(owner: Data, transaction: Data) throws -> TransferDeclaration {
            try TransferDeclaration(manifest: request.manifest, state: TransferState(
                transaction: transaction, owner: owner, storageGeneration: request.generation,
                contentHash: request.manifest.content.digest, length: request.manifest.length))
        }
        let bound = try transfer(owner: request.owner, transaction: request.transaction)
        XCTAssertTrue(request.matches(generation: request.generation, owner: request.owner,
            reviewed: request.reviewHash, transfer: bound))
        let foreign = Data(repeating: 9, count: 16)
        XCTAssertFalse(request.matches(generation: foreign, owner: request.owner,
            reviewed: request.reviewHash, transfer: bound))
        XCTAssertFalse(request.matches(generation: request.generation, owner: foreign,
            reviewed: request.reviewHash, transfer: bound))
        XCTAssertFalse(request.matches(generation: request.generation, owner: request.owner,
            reviewed: Data(repeating: 9, count: 32), transfer: bound))
        XCTAssertFalse(request.matches(generation: request.generation, owner: request.owner,
            reviewed: request.reviewHash, transfer: try transfer(owner: foreign, transaction: request.transaction)))
        XCTAssertFalse(request.matches(generation: request.generation, owner: request.owner,
            reviewed: request.reviewHash, transfer: try transfer(owner: request.owner, transaction: foreign)))
    }
}

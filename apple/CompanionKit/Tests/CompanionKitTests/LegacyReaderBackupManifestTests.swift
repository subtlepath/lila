import Foundation
import XCTest
@testable import CompanionKit

final class LegacyReaderBackupManifestTests: XCTestCase {
    private func fixture() throws -> Data {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        return try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/LegacyReaderBackupManifest-v1.fixture"))
    }
    func testSharedFixtureRoundTripsAndIncludesOriginalDayAndSessionFiles() throws {
        let bytes = try fixture()
        let decoded = try LegacyReaderBackupManifest(decoding: bytes)
        XCTAssertEqual(decoded.bytes, bytes)
        XCTAssertEqual(decoded.transaction, Data(repeating: 4, count: 16))
        XCTAssertEqual(Set(decoded.manifest.files.map(\.role)), [.reviews, .items, .profile, .days, .session])
        XCTAssertEqual(decoded.manifest.files.first(where: { $0.role == .days })?.length, 19)
    }
    func testRejectsSemanticCorruptionWithValidChecksum() throws {
        let bytes = try fixture()
        for offset in [0, 3, 68, 69, 70, 192] {
            var corrupt = bytes; corrupt[offset] ^= 0x80
            corrupt.replaceSubrange(432..<436, with: Data())
            corrupt.appendLittleEndian(UInt64(legacyCRC32(corrupt)), count: 4)
            XCTAssertThrowsError(try LegacyReaderBackupManifest(decoding: corrupt))
        }
        XCTAssertThrowsError(try LegacyReaderBackupManifest(decoding: bytes.dropLast()))
    }
}

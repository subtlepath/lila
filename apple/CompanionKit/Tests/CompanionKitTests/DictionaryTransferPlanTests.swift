import Foundation
import XCTest
@testable import CompanionKit

final class DictionaryTransferPlanTests: XCTestCase {
    func testDestinationUsesImmutableArchiveIdentityAndNeverFilename() throws {
        let id = try ContentID(String(repeating: "a", count: 64))
        for name in ["Dictionary.zip", "../escape.zip", "renamed.zip", "e\u{301}.zip"] {
            let content = LibraryContent(id: id, kind: .dictionary, length: 100, title: "Dictionary", originalFilename: name)
            let plan = try DictionaryTransferPlan(content: content)
            XCTAssertEqual(plan.destination, "/dictionaries/\(id.hex)/dictionary")
            XCTAssertLessThan(plan.destination.utf8.count, 128)
            XCTAssertEqual(plan.formatVersion, 1)
            let manifest = try ContentManifest(content: id, kind: .dictionary, length: 100, formatVersion: 1,
                                               logicalIdentity: Data(count: 16))
            let state = try TransferState(transaction: Data(repeating: 1, count: 16), owner: Data(repeating: 2, count: 16),
                storageGeneration: Data(repeating: 3, count: 16), contentHash: id.digest, length: 100)
            let frame = try TransferCommands.begin(TransferDeclaration(manifest: manifest, state: state), requestID: 1)
            XCTAssertEqual(Int(frame.payload[TransferDeclaration.encodedSize]), plan.destination.utf8.count)
            XCTAssertEqual(String(decoding: frame.payload.dropFirst(TransferDeclaration.encodedSize + 1), as: UTF8.self), plan.destination)
        }
    }
    func testRejectsUnsupportedKindsVersionsIdentitiesAndLengths() throws {
        let id = try ContentID(String(repeating: "a", count: 64))
        for length: UInt64 in [0, 21, UInt64(UInt32.max) + 1] {
            let manifest = try ContentManifest(content: id, kind: .dictionary, length: length,
                formatVersion: 1, logicalIdentity: Data(count: 16))
            XCTAssertThrowsError(try DictionaryTransferPlan(manifest: manifest))
        }
        for (kind, version, identity) in [(ContentKind.epub, UInt32(1), Data(count: 16)),
                                         (.dictionary, 2, Data(count: 16)), (.dictionary, 1, Data(repeating: 1, count: 16))] {
            let manifest = try ContentManifest(content: id, kind: kind, length: 100, formatVersion: version, logicalIdentity: identity)
            XCTAssertThrowsError(try DictionaryTransferPlan(manifest: manifest))
        }
    }
}

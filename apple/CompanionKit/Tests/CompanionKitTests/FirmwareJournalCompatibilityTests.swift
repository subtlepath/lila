import Foundation
import XCTest
@testable import CompanionKit

final class FirmwareJournalCompatibilityTests: XCTestCase {
    func testLegacyManifestCannotDowngradePersistedJournals() throws {
        let legacy = try JSONDecoder().decode(FirmwareJournalCompatibility.self, from: Data("{}".utf8))
        XCTAssertEqual(legacy.supportedJournalHeaderVersions, [])
        XCTAssertFalse(legacy.permits(readerHeaderVersions: nil))
        XCTAssertTrue(legacy.permits(readerHeaderVersions: []))
        XCTAssertFalse(legacy.permits(readerHeaderVersions: [1]))
        XCTAssertFalse(legacy.permits(readerHeaderVersions: [3]))
    }

    func testRequiresEveryPersistedFormatIncludingBackup() throws {
        let extendedOnly = try FirmwareJournalCompatibility(supportedJournalHeaderVersions: [3])
        XCTAssertTrue(extendedOnly.permits(readerHeaderVersions: [3]))
        XCTAssertFalse(extendedOnly.permits(readerHeaderVersions: [3, 2]))
        let complete = try FirmwareJournalCompatibility(supportedJournalHeaderVersions: [3, 1, 2])
        XCTAssertEqual(complete.supportedJournalHeaderVersions, [1, 2, 3])
        XCTAssertTrue(complete.permits(readerHeaderVersions: [3, 2, 2]))
        XCTAssertFalse(complete.permits(readerHeaderVersions: [4]))
        XCTAssertEqual(try JSONDecoder().decode(FirmwareJournalCompatibility.self,
                                               from: JSONEncoder().encode(complete)), complete)
    }

    func testRejectsMalformedAndUnknownDeclarations() {
        for json in ["{\"supportedJournalHeaderVersions\":[0]}",
                     "{\"supportedJournalHeaderVersions\":[4]}",
                     "{\"supportedJournalHeaderVersions\":[3,3]}",
                     "{\"supportedJournalHeaderVersions\":[true]}",
                     "{\"supportedJournalHeaderVersions\":\"3\"}"] {
            XCTAssertThrowsError(try JSONDecoder().decode(FirmwareJournalCompatibility.self, from: Data(json.utf8)))
        }
    }
    func testAuthenticatedFormatResponseValidationAndCompatibility() throws {
        let firmware = try FirmwareJournalCompatibility(supportedJournalHeaderVersions: [2, 3])
        for mask in UInt16(0) ... 255 {
            let reply = try ControlFrame(command: .journalFormats, response: true, requestID: 42,
                                         payload: Data([0, UInt8(mask)]))
            if mask < 8 {
                let formats = try JournalFormatStatus.decode(reply, requestID: 42)
                XCTAssertEqual(firmware.permits(readerHeaderVersions: formats), mask & 1 == 0)
            } else {
                XCTAssertThrowsError(try JournalFormatStatus.decode(reply, requestID: 42))
            }
        }
        for payload in [Data(), Data([0]), Data([1]), Data([2]), Data([0, 0, 0])] {
            let reply = try ControlFrame(command: .journalFormats, response: true, requestID: 42, payload: payload)
            XCTAssertThrowsError(try JournalFormatStatus.decode(reply, requestID: 42))
        }
        let wrongID = try ControlFrame(command: .journalFormats, response: true, requestID: 41, payload: Data([0, 6]))
        XCTAssertThrowsError(try JournalFormatStatus.decode(wrongID, requestID: 42))
        let request = try ControlFrame(command: .journalFormats, requestID: 42, payload: Data([0, 6]))
        XCTAssertThrowsError(try JournalFormatStatus.decode(request, requestID: 42))
    }

}

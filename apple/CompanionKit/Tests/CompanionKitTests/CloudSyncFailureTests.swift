import Foundation
import XCTest
@testable import CompanionKit

final class CloudSyncFailureTests: XCTestCase {
    func testLocalFailuresPreserveRecoveryCategory() throws {
        let identity = try EventIdentity(origin: Data(repeating: 1, count: 16), epoch: 1, sequence: 1)
        XCTAssertEqual(CloudSyncFailure.local(HistoryError.equivocation(identity)), .journalConflict)
        XCTAssertEqual(CloudSyncFailure.local(ProtocolError.truncated), .journalConflict)
        XCTAssertEqual(CloudSyncFailure.local(VaultError.integrity), .journalConflict)
        XCTAssertEqual(CloudSyncFailure.local(CloudSyncStateError.accountConfirmationRequired), .accountUnavailable)
        XCTAssertEqual(CloudSyncFailure.local(CloudSyncStateError.invalidState), .localPersistence)
        XCTAssertEqual(CloudSyncFailure.local(CocoaError(.fileWriteOutOfSpace)), .localPersistence)
        struct Unknown: Error {}
        XCTAssertEqual(CloudSyncFailure.local(Unknown()), .other)
    }
}

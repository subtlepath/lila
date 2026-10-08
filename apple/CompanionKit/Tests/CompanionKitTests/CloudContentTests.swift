import Foundation
import XCTest
@testable import CompanionKit

final class CloudContentTests: XCTestCase, @unchecked Sendable {
    private func content(_ digit: Character, kind: ContentKind = .epub, filename: String = "book.epub") throws -> LibraryContent {
        LibraryContent(id: try ContentID(String(repeating: String(digit), count: 64)), kind: kind,
                       length: 12, title: "Book", originalFilename: filename)
    }
    func testContentScanIsBoundedAndExcludesDeletedAndFirmware() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let first = try content("a"), second = try content("b"), removed = try content("c")
        let firmware = try content("d", kind: .firmware, filename: "firmware.bin")
        for item in [firmware, removed, second, first] { try await store.put(item) }
        _ = try await store.deleteLibraryContent(removed.id)
        let page = try await store.cloudContentPage(limit: 1); XCTAssertEqual(page, [first])
        let next = try await store.cloudContentPage(after: first.id, limit: 1); XCTAssertEqual(next, [second])
        let end = try await store.cloudContentPage(after: second.id, limit: 1); XCTAssertTrue(end.isEmpty)
        do { _ = try await store.cloudContentPage(limit: 129); XCTFail() }
        catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
    }
    func testContentReceiptsSurviveRestartAndRenamingAndRollbackMissingAssets() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let store = try LibraryStore(url: url)
        let first = try content("a"), second = try content("b"), missing = try content("c")
        try await store.put(first); try await store.put(second)
        try await store.acknowledgeCloudContent([CloudContentDescriptor(content: first)], account: "account")
        let reopened = try LibraryStore(url: url)
        let renamed = try content("a", filename: "renamed.epub")
        try await reopened.put(renamed)
        let pending = try await reopened.pendingCloudContent([renamed, second], account: "account")
        XCTAssertEqual(pending, [second])
        let other = try await reopened.pendingCloudContent([renamed, second], account: "other")
        XCTAssertEqual(other, [renamed, second])
        do {
            try await reopened.acknowledgeCloudContent([CloudContentDescriptor(content: second), CloudContentDescriptor(content: missing)], account: "account")
            XCTFail("Missing asset acknowledged")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let afterFailure = try await reopened.pendingCloudContent([second], account: "account")
        XCTAssertEqual(afterFailure, [second])
        try await reopened.acknowledgeCloudContent([CloudContentDescriptor(content: second)], account: "account")
        let repeated = try await reopened.pendingCloudContent([renamed, second], account: "account")
        XCTAssertTrue(repeated.isEmpty)
    }
}

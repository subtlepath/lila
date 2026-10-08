import Foundation
import XCTest
@testable import CompanionKit

final class PreparedContentWorkTests: XCTestCase, @unchecked Sendable {
    func testPreparationQueuesAllKindsAndReusesJobsAcrossRestart() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let store = try LibraryStore(url: url)
        let reader = Data(repeating: 1, count: 16), generation = Data(repeating: 2, count: 16)
        let owner = Data(repeating: 3, count: 16)
        for (index, kind) in [ContentKind.epub, .course, .font, .dictionary].enumerated() {
            let id = try ContentID(String(repeating: String(index + 1), count: 64))
            try await store.put(LibraryContent(id: id, kind: kind, length: 4, title: "Content", originalFilename: "asset"))
            _ = try await store.setReaderSelection(reader: reader, content: id, selected: true)
        }
        let inventory = try ReaderInventory(reader: reader, generation: generation, contents: [], complete: true)
        let first = try await store.prepareContentWork(reader: reader, generation: generation, installation: owner, inventory: inventory)
        XCTAssertEqual(first.count, 4)
        let jobs = try await store.pendingJobs()
        XCTAssertEqual(jobs.count, 4)
        XCTAssertTrue(first.allSatisfy { if case .transfer = $0 { return true }; return false })
        let reopened = try LibraryStore(url: url)
        let second = try await reopened.prepareContentWork(reader: reader, generation: generation, installation: owner, inventory: inventory)
        XCTAssertEqual(second, first)
        _ = try await reopened.setReaderSelection(reader: reader, content: jobs[0].content, selected: false)
        let third = try await reopened.prepareContentWork(reader: reader, generation: generation, installation: owner, inventory: inventory)
        XCTAssertTrue(third.contains(.abort(jobs[0])))
        let retained = try await reopened.pendingJobs()
        XCTAssertEqual(retained.count, 4)
    }
    func testIncompleteInventoryNeverQueuesWork() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
        let reader = Data(repeating: 1, count: 16), generation = Data(repeating: 2, count: 16)
        let inventory = try ReaderInventory(reader: reader, generation: generation, contents: [], complete: false)
        do {
            _ = try await store.prepareContentWork(reader: reader, generation: generation,
                installation: Data(repeating: 3, count: 16), inventory: inventory)
            XCTFail("Incomplete inventory accepted")
        } catch { XCTAssertEqual(error as? ContentReconciliationError, .incompleteInventory) }
        let jobs = try await store.pendingJobs(); XCTAssertTrue(jobs.isEmpty)
    }
}

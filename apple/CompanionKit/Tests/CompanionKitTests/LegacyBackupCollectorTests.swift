import Foundation
import XCTest
@testable import CompanionKit

private actor BackupWire: CompanionTransport {
    let manifest: LegacyReaderBackupManifest
    let files: [LegacyBackupRole: Data]
    var interrupted = false
    var offsets: [UInt32] = []
    var requests = 0
    init(manifest: LegacyReaderBackupManifest, files: [LegacyBackupRole: Data], interrupt: Bool = true) { self.manifest = manifest; self.files = files; interrupted = !interrupt }
    func reviewOffsets() -> [UInt32] { offsets }
    func requestCount() -> Int { requests }
    func exchange(_ frame: ControlFrame) async throws -> ControlFrame {
        requests += 1
        let request = try LegacyBackupRequest(decoding: frame.payload)
        var body = Data(), final = true
        if request.operation == .manifest { body = manifest.bytes }
        if request.operation == .file {
            let data = files[request.role!]!
            if request.role == .reviews {
                offsets.append(request.offset)
                if request.offset == 768 && !interrupted { interrupted = true; throw URLError(.networkConnectionLost) }
            }
            let start = Int(request.offset), end = min(data.count, start + Int(request.count))
            body = Data(data[start..<end]); final = end == data.count
        }
        var reply = Data([0x54, 0x4c, 0x53, 1, 0, request.bytes[5], final ? 1 : 0, 0])
        reply.append(request.transaction)
        reply.appendLittleEndian(UInt64(request.offset), count: 4)
        reply.appendLittleEndian(UInt64(body.count), count: 2)
        reply.append(contentsOf: [0, 0]); reply.append(body)
        return try ControlFrame(command: .exchangeChanges, response: true, requestID: frame.requestID, payload: reply)
    }
}
final class LegacyBackupCollectorTests: XCTestCase, @unchecked Sendable {
    func testInterruptedCollectionResumesAndRegistersOnlyVerifiedCompleteBackup() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let vault = try ContentVault(root: root.appendingPathComponent("vault"))
        let library = try LibraryStore(url: root.appendingPathComponent("library.sqlite"), random: { Data(repeating: 9, count: $0) })
        let files: [LegacyBackupRole: Data] = [.reviews: Data(repeating: 7, count: 2000), .items: Data(repeating: 8, count: 31), .profile: Data(repeating: 9, count: 80)]
        var receipts: [LegacyBackupFile] = []
        for role in [LegacyBackupRole.reviews, .items, .profile] {
            let path = root.appendingPathComponent(role.rawValue)
            try files[role]!.write(to: path)
            let object = try await vault.importFile(path)
            receipts.append(LegacyBackupFile(role: role, id: object.id, length: object.length))
        }
        let reader = Data(repeating: 1, count: 16), generation = Data(repeating: 2, count: 16)
        let course = Data(repeating: 3, count: 16), transaction = Data(repeating: 4, count: 16)
        let manifest = try LegacyBackupManifest(reader: reader, generation: generation, course: course, files: receipts)
        let pending = try await library.beginLegacyBackupExport(reader: reader, generation: generation, course: course,
            previousTransaction: transaction)
        XCTAssertEqual(pending, transaction)
        let reopened = try LibraryStore(url: root.appendingPathComponent("library.sqlite"), random: { Data(repeating: 6, count: $0) })
        let restored = try await reopened.beginLegacyBackupExport(reader: reader, generation: generation, course: course)
        XCTAssertEqual(restored, transaction)
        var oldSources: [LegacyBackupRole: URL] = [:]
        for role in files.keys { oldSources[role] = root.appendingPathComponent(role.rawValue) }
        let oldBackup = try await library.preserveLegacyBackup(manifest, sources: oldSources, vault: vault)
        do {
            try await library.completeLegacyBackupExport(reader: reader, generation: generation, course: course,
                transaction: transaction, backup: oldBackup)
            XCTFail("An older registration completed a pending transaction")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        let wire = BackupWire(manifest: try LegacyReaderBackupManifest(transaction: transaction, manifest: manifest), files: files, interrupt: false)
        let staging = root.appendingPathComponent("export-staging")
        do {
            _ = try await LegacyBackupCollector().collect(reader: reader, generation: generation, transport: wire,
                course: course, transaction: transaction, bound: true, capture: false, staging: staging,
                library: library, vault: vault, maximumBytes: 4096, progress: { value in
                    if value.role == .reviews { throw CancellationError() }
                })
            XCTFail("Interrupted export accepted")
        } catch { XCTAssertTrue(error is CancellationError) }
        let before = try await library.legacyBackupIDs(reader: reader, generation: generation, course: course)
        XCTAssertEqual(before, [oldBackup])
        let key = [reader, generation, course, transaction].map { $0.map { String(format: "%02x", $0) }.joined() }.joined(separator: "-")
        let stagedManifest = staging.appendingPathComponent(key).appendingPathComponent("manifest")
        var partialManifest = Data(try Data(contentsOf: stagedManifest).prefix(100))
        partialManifest[0] ^= 1
        try partialManifest.write(to: stagedManifest)
        let beforeConflict = await wire.reviewOffsets()
        do {
            _ = try await LegacyBackupCollector().collect(reader: reader, generation: generation, transport: wire,
                course: course, transaction: transaction, bound: true, capture: false, staging: staging,
                library: library, vault: vault, maximumBytes: 4096)
            XCTFail("Mismatching partial manifest accepted")
        } catch { XCTAssertEqual(error as? LegacyBackupCollectorError, .conflictingStage) }
        XCTAssertEqual(try Data(contentsOf: stagedManifest), partialManifest)
        let afterConflict = await wire.reviewOffsets()
        XCTAssertEqual(beforeConflict, afterConflict)
        partialManifest[0] ^= 1
        try partialManifest.write(to: stagedManifest)
        let id = try await LegacyBackupCollector().collect(reader: reader, generation: generation, transport: wire,
            course: course, transaction: transaction, bound: true, capture: false, staging: staging,
            library: library, vault: vault, maximumBytes: 4096)
        let verified = try await vault.verifiedLegacyBackup(id)
        XCTAssertEqual(verified, manifest)
        do {
            try await reopened.completeLegacyBackupExport(reader: reader, generation: generation, course: course,
                transaction: Data(repeating: 9, count: 16), backup: id)
            XCTFail("Foreign transaction completed the job")
        } catch { XCTAssertEqual(error as? StoreError, .invalidValue) }
        try await reopened.completeLegacyBackupExport(reader: reader, generation: generation, course: course,
            transaction: transaction, backup: id)
        try await reopened.completeLegacyBackupExport(reader: reader, generation: generation, course: course,
            transaction: transaction, backup: id)
        let offsetsBeforeRetry = await wire.reviewOffsets()
        let requestsBeforeRetry = await wire.requestCount()
        let cached = try await LegacyBackupCollector().collect(reader: reader, generation: generation, transport: wire,
            course: course, transaction: transaction, bound: true, capture: true, staging: staging,
            library: reopened, vault: vault, maximumBytes: 4096)
        XCTAssertEqual(cached, id)
        let offsetsAfterRetry = await wire.reviewOffsets()
        XCTAssertEqual(offsetsAfterRetry, offsetsBeforeRetry)
        let requestsAfterRetry = await wire.requestCount()
        XCTAssertEqual(requestsAfterRetry, requestsBeforeRetry)
        let reviewObject = try await vault.verifiedObject(receipts.first(where: { $0.role == .reviews })!.id)
        try Data(repeating: 1, count: 2000).write(to: reviewObject.url)
        do {
            _ = try await LegacyBackupCollector().collect(reader: reader, generation: generation, transport: wire,
                course: course, transaction: transaction, bound: true, capture: true, staging: staging,
                library: reopened, vault: vault, maximumBytes: 4096)
            XCTFail("Corrupt completed vault receipt accepted")
        } catch { XCTAssertEqual(error as? VaultError, .integrity) }
        let offsetsAfterCorruption = await wire.reviewOffsets()
        XCTAssertEqual(offsetsAfterCorruption, offsetsBeforeRetry)
        let requestsAfterCorruption = await wire.requestCount()
        XCTAssertEqual(requestsAfterCorruption, requestsBeforeRetry)
        let next = try await reopened.beginLegacyBackupExport(reader: reader, generation: generation, course: course)
        XCTAssertNotEqual(next, transaction)
        let offsets = await wire.reviewOffsets()
        XCTAssertEqual(offsets, [0, 768, 1536])
    }
}


extension LegacyBackupCollectorTests {
    func testConcurrentConnectionsReserveOnePendingTransaction() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let database = root.appendingPathComponent("library.sqlite")
        let first = try LibraryStore(url: database, random: { Data(repeating: 4, count: $0) })
        let second = try LibraryStore(url: database, random: { Data(repeating: 5, count: $0) })
        let reader = Data(repeating: 1, count: 16), generation = Data(repeating: 2, count: 16)
        for index in 1...12 {
            let course = Data(repeating: UInt8(index), count: 16)
            async let a = first.beginLegacyBackupExport(reader: reader, generation: generation, course: course)
            async let b = second.beginLegacyBackupExport(reader: reader, generation: generation, course: course)
            let values = try await (a, b)
            XCTAssertEqual(values.0, values.1)
            let retained = try await second.beginLegacyBackupExport(reader: reader, generation: generation, course: course,
                previousTransaction: Data(repeating: 9, count: 16))
            XCTAssertEqual(retained, values.0)
        }
    }
}

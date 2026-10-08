import Foundation
import XCTest
@testable import CompanionKit

private actor InventoryReaderFixture: CompanionTransport {
    private let wrongID: Bool
    private let changedRevision: Bool
    private let disconnect: Bool
    private let remoteStatus: UInt8?
    private let errorCommand: Bool
    private var calls = 0
    init(wrongID: Bool = false, changedRevision: Bool = false, disconnect: Bool = false,
         remoteStatus: UInt8? = nil, errorCommand: Bool = false) {
        self.wrongID = wrongID; self.changedRevision = changedRevision; self.disconnect = disconnect
        self.remoteStatus = remoteStatus; self.errorCommand = errorCommand
    }
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        calls += 1
        if disconnect && calls == 2 { throw URLError(.networkConnectionLost) }
        if let remoteStatus, calls == 2 {
            return try ControlFrame(command: errorCommand ? .error : .inventory, response: true,
                                    requestID: request.requestID, payload: Data([remoteStatus]))
        }
        var input = ByteReader(request.payload)
        guard try input.number(1) == 1, try input.take(16) == Data(repeating: 2, count: 16),
              try input.number(8) == (calls == 1 ? 0 : 1), try input.number(8) == UInt64(calls - 1) else { throw ProtocolError.value }
        var page = Data([0, 1]); page.append(Data(repeating: 2, count: 16))
        page.appendLittleEndian(changedRevision && calls == 2 ? 2 : 1, count: 8)
        page.appendLittleEndian(UInt64(calls - 1), count: 8); page.appendLittleEndian(calls == 1 ? 1 : 0, count: 8)
        page.append(calls == 2 ? 1 : 0); page.append(1)
        page.append(contentsOf: [1, 2]); page.append(Data(repeating: UInt8(calls), count: 32)); page.append(1)
        page.appendLittleEndian(3, count: 8); page.appendLittleEndian(1, count: 4); page.append(Data(count: 16))
        return try ControlFrame(command: .inventory, response: true, requestID: request.requestID + (wrongID ? 1 : 0), payload: page)
    }
}
final class InventoryCollectorTests: XCTestCase, @unchecked Sendable {
    func testInvalidatedReaderInventoryReturnsNoPartialSnapshotAndCanCollectAfterReconnect() async throws {
        let collector = InventoryCollector()
        for (status, errorCommand, expected, reopen) in [
            (UInt8(4), false, InventoryCollectorError.remote(4), true),
            (UInt8(5), false, InventoryCollectorError.remote(5), false),
            (UInt8(4), true, InventoryCollectorError.control(4), false)
        ] {
            do {
                _ = try await collector.collect(device: device(), transport:
                    InventoryReaderFixture(remoteStatus: status, errorCommand: errorCommand), maximumEntries: 2)
                XCTFail("failed paging must not expose the accepted first page")
            } catch let error as InventoryCollectorError {
                XCTAssertEqual(error, expected)
                XCTAssertEqual(error.requiresReaderReopen, reopen)
            }
        }
        let recovered = try await collector.collect(device: device(), transport: InventoryReaderFixture(), maximumEntries: 2)
        XCTAssertTrue(recovered.complete)
        XCTAssertEqual(recovered.contents.count, 2)
    }
    private func device() throws -> DeviceDescriptor {
        var bytes = Data([1, 1]); bytes.append(Data(repeating: 1, count: 16)); bytes.append(Data(repeating: 2, count: 16))
        bytes.append(contentsOf: [1, 0, 0, 0, 0, 80, 1, 1]); bytes.append(Data(count: 32))
        return try DeviceDescriptor(decoding: bytes)
    }
    func testRequestsFollowAcceptedSnapshotAndReturnCompleteInventory() async throws {
        let inventory = try await InventoryCollector().collect(device: device(), transport: InventoryReaderFixture(), maximumEntries: 2)
        XCTAssertTrue(inventory.complete); XCTAssertEqual(inventory.contents.count, 2)
        XCTAssertEqual(inventory.reader, Data(repeating: 1, count: 16))
    }
    func testBrokenRepliesAndInterruptedScansReturnNoInventory() async throws {
        let collector = InventoryCollector()
        do { _ = try await collector.collect(device: device(), transport: InventoryReaderFixture(wrongID: true), maximumEntries: 2); XCTFail() }
        catch { XCTAssertEqual(error as? InventoryCollectorError, .invalidResponse) }
        do { _ = try await collector.collect(device: device(), transport: InventoryReaderFixture(changedRevision: true), maximumEntries: 2); XCTFail() }
        catch { XCTAssertEqual(error as? InventoryError, .changedSnapshot) }
        do { _ = try await collector.collect(device: device(), transport: InventoryReaderFixture(disconnect: true), maximumEntries: 2); XCTFail() }
        catch { XCTAssertEqual((error as? URLError)?.code, .networkConnectionLost) }
        let recovered = try await collector.collect(device: device(), transport: InventoryReaderFixture(), maximumEntries: 2)
        XCTAssertTrue(recovered.complete)
    }
}

import Foundation
import XCTest
@testable import CompanionKit
final class FirmwareReaderInfoTests: XCTestCase {
    func testSharedFixtureAndBoundReply() throws {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        let bytes = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/FirmwareReaderInfo-v1.fixture"))
        let info = try FirmwareReaderInfo(decoding: bytes)
        XCTAssertEqual(info.partitionBytes, 0x900000)
        XCTAssertEqual(info.chip, 5)
        XCTAssertEqual(info.stateSchema, 1)
        XCTAssertEqual(info.headerVersions, [1, 2])
        XCTAssertEqual(info.battery, 60)
        var descriptorBytes = Data([1, 1]) + Data(repeating: 4, count: 16) + info.generation
        descriptorBytes.append(contentsOf: [1, 3, 0, 0, 0, 99, 1, 1])
        descriptorBytes.append(info.runningBuild)
        let descriptor = try DeviceDescriptor(decoding: descriptorBytes)
        let fresh = try info.refreshedDevice(descriptor)
        XCTAssertEqual(fresh.batteryPercent, 60)
        XCTAssertEqual(fresh.identity, descriptor.identity)
        XCTAssertEqual(fresh.capabilities, descriptor.capabilities)
        descriptorBytes[42] ^= 1
        XCTAssertThrowsError(try info.refreshedDevice(DeviceDescriptor(decoding: descriptorBytes)))
        let request = try FirmwareReaderInfo.request(generation: info.generation, requestID: 9)
        let reply = try ControlFrame(command: .installFirmware, response: true, requestID: 9, payload: bytes)
        XCTAssertEqual(try FirmwareReaderInfo.decode(reply, request: request), info)
        let foreign = try FirmwareReaderInfo.request(generation: Data(repeating: 3, count: 16), requestID: 9)
        XCTAssertThrowsError(try FirmwareReaderInfo.decode(reply, request: foreign))
        for offset in [3, 4, 5, 58, 59, 72] {
            var invalid = bytes; invalid[offset] = 0xff
            XCTAssertThrowsError(try FirmwareReaderInfo(decoding: invalid))
        }
        XCTAssertThrowsError(try FirmwareReaderInfo(decoding: bytes.dropLast()))
    }
}

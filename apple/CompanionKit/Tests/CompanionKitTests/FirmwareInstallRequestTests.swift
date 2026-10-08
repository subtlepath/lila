import Foundation
import XCTest
@testable import CompanionKit
final class FirmwareInstallRequestTests: XCTestCase {
    func testSharedFixtureRejectsMalformedConstraints() throws {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        let bytes = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/FirmwareInstallRequest-v1.fixture"))
        let install = try FirmwareInstallRequest(decoding: bytes)
        XCTAssertEqual(install.bytes, bytes)
        let request = try install.controlFrame(requestID: 7)
        let payload = Data([0]) + bytes.subdata(in: 20..<36)
        try install.validateAcceptance(ControlFrame(command: .installFirmware, response: true,
            requestID: 7, payload: payload), request: request)
        for result in UInt8(1)...5 {
            XCTAssertThrowsError(try install.validateAcceptance(ControlFrame(command: .installFirmware,
                response: true, requestID: 7, payload: Data([result]) + payload.dropFirst()), request: request)) {
                XCTAssertEqual($0 as? FirmwareInstallCommandError, .rejected(result))
            }
        }
        for bad in [
            try ControlFrame(command: .installFirmware, response: true, requestID: 8, payload: payload),
            try ControlFrame(command: .installFirmware, response: false, requestID: 7, payload: payload),
            try ControlFrame(command: .installFirmware, response: true, requestID: 7, payload: Data([0]) + Data(repeating: 9, count: 16)),
            try ControlFrame(command: .installFirmware, response: true, requestID: 7, payload: Data([6]) + payload.dropFirst())
        ] { XCTAssertThrowsError(try install.validateAcceptance(bad, request: request)) }
        XCTAssertThrowsError(try install.validateAcceptance(ControlFrame(command: .error, response: true,
            requestID: 7, payload: Data([2])), request: request)) {
            XCTAssertEqual($0 as? FirmwareInstallCommandError, .control(2))
        }
        for offset in [3, 84, 88, 90, 91, 92, 94, 95] {
            var bad = bytes; bad[offset] = 0xff
            XCTAssertThrowsError(try FirmwareInstallRequest(decoding: bad))
        }
        for range in [4..<20, 20..<36, 36..<68] {
            var bad = bytes; bad.replaceSubrange(range, with: Data(count: range.count))
            XCTAssertThrowsError(try FirmwareInstallRequest(decoding: bad))
        }
        XCTAssertThrowsError(try FirmwareInstallRequest(decoding: bytes.dropLast()))
    }
}

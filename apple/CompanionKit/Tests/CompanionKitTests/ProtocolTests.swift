import Foundation
import XCTest
@testable import CompanionKit

final class ProtocolTests: XCTestCase {
    private func fixture(_ name: String) throws -> Data {
        var root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        for _ in 0 ..< 4 { root.deleteLastPathComponent() }
        let json = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/\(name).json"))
        let object = try XCTUnwrap(JSONSerialization.jsonObject(with: json) as? [String: Any])
        let hex = Array(try XCTUnwrap(object["binaryHex"] as? String))
        var bytes = Data()
        for index in stride(from: 0, to: hex.count, by: 2) {
            bytes.append(try XCTUnwrap(UInt8(String(hex[index ... index + 1]), radix: 16)))
        }
        return bytes
    }
    func testSharedCourseCapabilityFixture() throws {
        let descriptor = try DeviceDescriptor(decoding: fixture("CourseTransferCapabilities"))
        XCTAssertEqual(descriptor.readerCapabilities, [.declaredTransfers, .courseTransfers])
        XCTAssertTrue(descriptor.readerCapabilities.supportsCourseTransfer)
        XCTAssertTrue(ReaderCapabilities(rawValue: UInt32.max).supportsCourseTransfer)
        for capabilities: UInt32 in [0, 1, 2, 4, UInt32.max & ~UInt32(3)] {
            XCTAssertFalse(ReaderCapabilities(rawValue: capabilities).supportsCourseTransfer)
        }
    }
    func testSharedDeclaredBeginTransferFixture() throws {
        let declaration = try TransferDeclaration(decoding: fixture("TransferDeclaration"))
        let request = try TransferCommands.begin(declaration, requestID: 42)
        XCTAssertEqual(request.command, .beginTransfer)
        XCTAssertEqual(request.payload, try fixture("DeclaredBeginTransfer"))
        XCTAssertEqual(try ControlFrame(decoding: request.encoded(), authenticated: true), request)
    }
    func testSharedCourseTransferDeclarationFixture() throws {
        let bytes = try fixture("TransferDeclaration")
        let declaration = try TransferDeclaration(decoding: bytes)
        XCTAssertEqual(declaration.manifest.kind, .course)
        XCTAssertEqual(declaration.manifest.length, 2000)
        XCTAssertEqual(declaration.manifest.logicalIdentity, Data(repeating: 7, count: 16))
        XCTAssertEqual(declaration.encoded, bytes)
    }
    func testAllFirmwareFixturesAndEveryTruncatedPrefix() throws {
        let names = ["DeviceDescriptor", "ContentManifest", "SyncEvent", "SyncCheckpoint", "TransferState"]
        for (index, name) in names.enumerated() {
            let bytes = try fixture(name)
            let record = try RecordEnvelope(decoding: bytes)
            XCTAssertEqual(record.kind.rawValue, UInt8(index + 1))
            XCTAssertEqual(record.bytes, bytes)
            for count in 0 ..< bytes.count { XCTAssertThrowsError(try RecordEnvelope(decoding: Data(bytes.prefix(count)))) }
            XCTAssertThrowsError(try RecordEnvelope(decoding: bytes + Data([0])))
        }
        let descriptor = try DeviceDescriptor(decoding: fixture("DeviceDescriptor"))
        XCTAssertEqual(descriptor.board, .x4)
        XCTAssertEqual(descriptor.batteryPercent, 75)
        XCTAssertEqual(descriptor.capabilities, 15)
        XCTAssertEqual(descriptor.identity, Data((1 ... 16).map { UInt8($0) }))
        let state = try TransferState(decoding: fixture("TransferState"))
        XCTAssertEqual(state.length, 123456)
        XCTAssertEqual(state.durableOffset, 8192)
        XCTAssertEqual(state.phase, .receiving)
        XCTAssertEqual(state.encoded(), try fixture("TransferState"))
    }
    func testFramesAuthenticationAndEveryFragmentBoundary() throws {
        for command in Command.allCases {
            let frame = try ControlFrame(command: command, requestID: 0x12345678, payload: Data([1, 2, 3]))
            let bytes = frame.encoded()
            XCTAssertEqual(try ControlFrame(decoding: bytes, authenticated: true), frame)
            let prefixed = Data([255]) + bytes
            XCTAssertEqual(try ControlFrame(decoding: prefixed.dropFirst(), authenticated: true), frame)
            if command != .discover { XCTAssertThrowsError(try ControlFrame(decoding: bytes, authenticated: false)) }
            for split in 1 ..< bytes.count {
                var assembler = FrameAssembler()
                XCTAssertNil(try assembler.append(Data(bytes.prefix(split)), authenticated: true))
                XCTAssertEqual(try assembler.append(Data(bytes.dropFirst(split)), authenticated: true), frame)
            }
            for size in 0 ..< bytes.count { XCTAssertThrowsError(try ControlFrame(decoding: Data(bytes.prefix(size)), authenticated: true)) }
        }
    }
    func testMalformedHeadersAndOversizedBodies() throws {
        let original = try ControlFrame(command: .discover, requestID: 1).encoded()
        for (offset, value) in [(0, 0), (2, 2), (3, 255), (4, 2), (5, 1), (11, 5)] {
            var bytes = original
            bytes[offset] = UInt8(value)
            XCTAssertThrowsError(try ControlFrame(decoding: bytes, authenticated: true))
            var assembler = FrameAssembler()
            XCTAssertThrowsError(try assembler.append(bytes, authenticated: true))
        }
        XCTAssertThrowsError(try ControlFrame(command: .transferChunk, requestID: 1, payload: Data(repeating: 0, count: 1025)))
        var assembler = FrameAssembler()
        XCTAssertThrowsError(try assembler.append(Data(repeating: 0, count: 1037), authenticated: true))
    }
}

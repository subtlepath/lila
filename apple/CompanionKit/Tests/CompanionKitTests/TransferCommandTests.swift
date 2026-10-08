import Foundation
import XCTest
@testable import CompanionKit

final class TransferCommandTests: XCTestCase {
    func testTransferDeclarationBindsCourseFamilyKindHashAndLength() throws {
        let state = try initial()
        let id = try ContentID(String(repeating: "04", count: 32))
        let manifest = try ContentManifest(content: id, kind: .course, length: state.length,
            formatVersion: 1, logicalIdentity: Data(repeating: 7, count: 16))
        let declaration = try TransferDeclaration(manifest: manifest, state: state)
        XCTAssertEqual(declaration.encoded.count, 162)
        XCTAssertEqual(try TransferDeclaration(decoding: declaration.encoded), declaration)
        XCTAssertThrowsError(try TransferDeclaration(manifest: manifest, state: initial(offset: 1)))
        XCTAssertThrowsError(try TransferDeclaration(manifest: manifest, state: initial(phase: .committed)))
        for length in [UInt64(0), state.length + 1] {
            let wrong = try ContentManifest(content: id, kind: .course, length: length, formatVersion: 1,
                                            logicalIdentity: manifest.logicalIdentity)
            XCTAssertThrowsError(try TransferDeclaration(manifest: wrong, state: state))
        }
        let wrongID = try ContentManifest(content: ContentID(String(repeating: "0", count: 64)), kind: .course,
            length: state.length, formatVersion: 1, logicalIdentity: manifest.logicalIdentity)
        XCTAssertThrowsError(try TransferDeclaration(manifest: wrongID, state: state))
        let unbound = try ContentManifest(content: id, kind: .course, length: state.length, formatVersion: 1,
                                          logicalIdentity: Data(count: 16))
        XCTAssertThrowsError(try TransferDeclaration(manifest: unbound, state: state))
        let wrongKind = try ContentManifest(content: id, kind: .epub, length: state.length, formatVersion: 1,
                                            logicalIdentity: manifest.logicalIdentity)
        XCTAssertThrowsError(try TransferDeclaration(manifest: wrongKind, state: state))
        for count in 0..<declaration.encoded.count {
            XCTAssertThrowsError(try TransferDeclaration(decoding: Data(declaration.encoded.prefix(count))))
        }
        XCTAssertThrowsError(try TransferDeclaration(decoding: declaration.encoded + Data([0])))
    }
    func testDeclaredEPUBUsesCanonicalHashPathAndUnsupportedKindsStayBlocked() throws {
        let state = try initial()
        let id = try ContentID(String(repeating: "04", count: 32))
        let manifest = try ContentManifest(content: id, kind: .epub, length: state.length,
            formatVersion: 0, logicalIdentity: Data(count: 16))
        let declaration = try TransferDeclaration(manifest: manifest, state: state)
        let request = try TransferCommands.begin(declaration, requestID: 43)
        XCTAssertEqual(Data(request.payload.prefix(162)), declaration.encoded)
        let destination = Data(("/Books/Companion/" + id.hex + ".epub").utf8)
        XCTAssertEqual(request.payload[162], UInt8(destination.count))
        XCTAssertEqual(Data(request.payload.dropFirst(163)), destination)
        for kind in [ContentKind.font, .firmware] {
            let unsupported = try ContentManifest(content: id, kind: kind, length: state.length,
                formatVersion: 1, logicalIdentity: Data(count: 16))
            let contract = try TransferDeclaration(manifest: unsupported, state: state)
            XCTAssertThrowsError(try TransferCommands.begin(contract, requestID: 44)) {
                XCTAssertEqual($0 as? TransferRunnerError, .unsupportedContent)
            }
        }
    }
    private func initial(offset: UInt64 = 0, phase: TransferPhase = .receiving, owner: UInt8 = 2) throws -> TransferState {
        try TransferState(transaction: Data(repeating: 1, count: 16), owner: Data(repeating: owner, count: 16),
                          storageGeneration: Data(repeating: 3, count: 16), contentHash: Data(repeating: 4, count: 32),
                          length: 2000, durableOffset: offset, phase: phase)
    }
    func testStateRoundTripAndCanonicalBeginPayload() throws {
        let state = try initial()
        XCTAssertEqual(try TransferState(decoding: state.encoded()), state)
        let request = try TransferCommands.begin(state, requestID: 42)
        XCTAssertEqual(request.command, .beginTransfer)
        XCTAssertEqual(request.payload.prefix(99), state.encoded())
        let path = "/Books/Companion/" + String(repeating: "04", count: 32) + ".epub"
        XCTAssertEqual(request.payload[99], UInt8(path.utf8.count))
        XCTAssertEqual(String(decoding: request.payload.dropFirst(100), as: UTF8.self), path)
        XCTAssertThrowsError(try TransferCommands.begin(initial(offset: 1), requestID: 1))
    }
    func testChunkBoundariesAndTransactionCommands() throws {
        let state = try initial()
        let chunk = try TransferCommands.chunk(transaction: state.transaction, offset: 0x0102030405060708,
                                               bytes: Data(repeating: 7, count: 1000), requestID: 1)
        XCTAssertEqual(chunk.payload.count, 1024)
        XCTAssertEqual(Array(chunk.payload[16 ..< 24]), [8, 7, 6, 5, 4, 3, 2, 1])
        for count in [0, 1001] {
            XCTAssertThrowsError(try TransferCommands.chunk(transaction: state.transaction, offset: 0, bytes: Data(count: count), requestID: 1))
        }
        for command in [Command.transferStatus, .commit, .abort] {
            XCTAssertEqual(try TransferCommands.transaction(command, identity: state.transaction, requestID: 4).payload, state.transaction)
        }
        XCTAssertThrowsError(try TransferCommands.transaction(.discover, identity: state.transaction, requestID: 4))
    }
    func testResponseBindingAndRecoveredBackwardOffset() throws {
        let expected = try initial(offset: 1000)
        let request = try TransferCommands.transaction(.transferStatus, identity: expected.transaction, requestID: 8)
        let recovered = try initial(offset: 500)
        let frame = try ControlFrame(command: .transferStatus, response: true, requestID: 8,
                                     payload: Data([0]) + recovered.encoded())
        XCTAssertEqual(try TransferCommands.response(frame, to: request, expected: expected).durableOffset, 500)
        let foreign = try ControlFrame(command: .transferStatus, response: true, requestID: 8,
                                       payload: Data([0]) + initial(owner: 9).encoded())
        XCTAssertThrowsError(try TransferCommands.response(foreign, to: request, expected: expected)) { error in
            XCTAssertEqual(error as? TransferCommandError, .identityMismatch)
        }
        for response in [try ControlFrame(command: .transferStatus, requestID: 8, payload: frame.payload),
                         try ControlFrame(command: .transferStatus, response: true, requestID: 9, payload: frame.payload),
                         try ControlFrame(command: .commit, response: true, requestID: 8, payload: frame.payload)] {
            XCTAssertThrowsError(try TransferCommands.response(response, to: request, expected: expected))
        }
    }
    func testErrorsAndMalformedSuccessResponses() throws {
        let expected = try initial()
        let request = try TransferCommands.begin(expected, requestID: 1)
        for result in UInt8(1) ... 9 {
            let frame = try ControlFrame(command: .beginTransfer, response: true, requestID: 1, payload: Data([result]))
            XCTAssertThrowsError(try TransferCommands.response(frame, to: request, expected: expected)) { error in
                XCTAssertEqual(error as? TransferCommandError, .remote(TransferResult(rawValue: result)!))
            }
        }
        let commit = try TransferCommands.transaction(.commit, identity: expected.transaction, requestID: 2)
        let unfinished = try ControlFrame(command: .commit, response: true, requestID: 2, payload: Data([0]) + expected.encoded())
        XCTAssertThrowsError(try TransferCommands.response(unfinished, to: commit, expected: expected))
        let committed = try initial(offset: 2000, phase: .committed)
        let finished = try ControlFrame(command: .commit, response: true, requestID: 2, payload: Data([0]) + committed.encoded())
        XCTAssertEqual(try TransferCommands.response(finished, to: commit, expected: expected).phase, .committed)
        for payload in [Data(), Data([0]), Data([10]), Data([1, 0])] {
            let frame = try ControlFrame(command: .beginTransfer, response: true, requestID: 1, payload: payload)
            XCTAssertThrowsError(try TransferCommands.response(frame, to: request, expected: expected))
        }
    }
    func testFirmwareStagingUsesFixedDestinationAndSupportedFormat() throws {
        let state = try TransferState(transaction: Data(repeating: 1, count: 16), owner: Data(repeating: 2, count: 16),
            storageGeneration: Data(repeating: 3, count: 16), contentHash: Data(repeating: 4, count: 32), length: 65536)
        let manifest = try ContentManifest(content: ContentID(String(repeating: "04", count: 32)), kind: .firmware,
            length: state.length, formatVersion: 1, logicalIdentity: Data(count: 16))
        let begin = try TransferCommands.begin(TransferDeclaration(manifest: manifest, state: state), requestID: 8)
        XCTAssertEqual(begin.command, .beginTransfer)
        XCTAssertEqual(Data(begin.payload.dropFirst(163)), Data("/Companion/firmware.bin".utf8))
        let wrong = try ContentManifest(content: manifest.content, kind: .firmware, length: state.length,
            formatVersion: 2, logicalIdentity: Data(count: 16))
        XCTAssertThrowsError(try TransferCommands.begin(TransferDeclaration(manifest: wrong, state: state), requestID: 8))
    }

}

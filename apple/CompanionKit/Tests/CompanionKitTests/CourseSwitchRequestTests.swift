import Foundation
import XCTest
@testable import CompanionKit

final class CourseSwitchRequestTests: XCTestCase {
    func testSharedFixtureRejectsUnboundAndSameCourseConsent() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let bytes = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/CourseSwitchRequest-v1.fixture"))
        let request = try CourseSwitchRequest(decoding: bytes)
        XCTAssertEqual(request.bytes, bytes)
        XCTAssertEqual(request.generation, Data(repeating: 1, count: 16))
        XCTAssertEqual(request.transaction, Data(repeating: 2, count: 16))
        XCTAssertEqual(request.previousCourse, Data(repeating: 3, count: 16))
        XCTAssertEqual(request.nextCourse, Data(repeating: 4, count: 16))
        XCTAssertEqual(request.previousHash, Data(repeating: 5, count: 32))
        XCTAssertEqual(request.nextHash, Data(repeating: 6, count: 32))
        for range in [4..<20, 20..<36, 36..<52, 52..<68, 68..<100, 100..<132] {
            var invalid = bytes
            invalid.replaceSubrange(range, with: Data(repeating: 0, count: range.count))
            XCTAssertThrowsError(try CourseSwitchRequest(decoding: invalid))
        }
        var invalid = bytes
        invalid.replaceSubrange(52..<68, with: request.previousCourse)
        XCTAssertThrowsError(try CourseSwitchRequest(decoding: invalid))
        invalid = bytes
        invalid.replaceSubrange(100..<132, with: request.previousHash)
        XCTAssertThrowsError(try CourseSwitchRequest(decoding: invalid))
        invalid = bytes; invalid[3] = 2
        XCTAssertThrowsError(try CourseSwitchRequest(decoding: invalid))
        XCTAssertThrowsError(try CourseSwitchRequest(decoding: bytes.dropLast()))
        XCTAssertThrowsError(try CourseSwitchRequest(decoding: bytes + Data([0])))
    }
    func testConsentCannotBeRetargeted() throws {
        let request = try CourseSwitchRequest(generation: Data(repeating: 1, count: 16),
            transaction: Data(repeating: 2, count: 16), previousCourse: Data(repeating: 3, count: 16),
            nextCourse: Data(repeating: 4, count: 16), previousHash: Data(repeating: 5, count: 32),
            nextHash: Data(repeating: 6, count: 32))
        func manifest(course: Data, hash: Data, kind: ContentKind = .course,
                      length: UInt64 = 100, version: UInt32 = 1) throws -> ContentManifest {
            try ContentManifest(content: ContentID(hash.map { String(format: "%02x", $0) }.joined()),
                kind: kind, length: length, formatVersion: version, logicalIdentity: course)
        }
        let current = try manifest(course: request.previousCourse, hash: request.previousHash)
        let proposed = try manifest(course: request.nextCourse, hash: request.nextHash)
        func declaration(transaction: Data? = nil, generation: Data? = nil,
                         proposed: ContentManifest? = nil) throws -> TransferDeclaration {
            let pack = try proposed ?? manifest(course: request.nextCourse, hash: request.nextHash)
            return try TransferDeclaration(manifest: pack, state: TransferState(
                transaction: transaction ?? request.transaction, owner: Data(repeating: 7, count: 16),
                storageGeneration: generation ?? request.generation, contentHash: pack.content.digest, length: pack.length))
        }
        let next = try declaration(proposed: proposed)
        XCTAssertTrue(request.matches(generation: request.generation, current: current, next: next))
        let foreign = Data(repeating: 9, count: 16)
        XCTAssertFalse(request.matches(generation: foreign, current: current, next: next))
        XCTAssertFalse(request.matches(generation: request.generation, current: current,
            next: try declaration(transaction: foreign)))
        XCTAssertFalse(request.matches(generation: request.generation, current: current,
            next: try declaration(generation: foreign)))
        for altered in [try manifest(course: foreign, hash: request.previousHash),
                        try manifest(course: request.previousCourse, hash: request.nextHash),
                        try manifest(course: request.previousCourse, hash: request.previousHash, kind: .epub),
                        try manifest(course: request.previousCourse, hash: request.previousHash, length: 0),
                        try manifest(course: request.previousCourse, hash: request.previousHash, version: 0)] {
            XCTAssertFalse(request.matches(generation: request.generation, current: altered, next: next))
        }
        for altered in [try manifest(course: foreign, hash: request.nextHash),
                        try manifest(course: request.nextCourse, hash: request.previousHash)] {
            XCTAssertFalse(request.matches(generation: request.generation, current: current,
                next: try declaration(proposed: altered)))
        }
    }

    func testReplyBindsRequestAndTransaction() throws {
        let consent = try CourseSwitchRequest(generation: Data(repeating: 1, count: 16),
            transaction: Data(repeating: 2, count: 16), previousCourse: Data(repeating: 3, count: 16),
            nextCourse: Data(repeating: 4, count: 16), previousHash: Data(repeating: 5, count: 32),
            nextHash: Data(repeating: 6, count: 32))
        let request = try ControlFrame(command: .exchangeChanges, requestID: 7, payload: consent.bytes)
        let body = Data([TransferResult.ok.rawValue]) + consent.transaction
        let good = try ControlFrame(command: .exchangeChanges, response: true, requestID: 7, payload: body)
        XCTAssertNoThrow(try CourseSwitchReply.validate(good, request: request))
        for reply in [try ControlFrame(command: .exchangeChanges, response: true, requestID: 8, payload: body),
                      try ControlFrame(command: .exchangeChanges, response: false, requestID: 7, payload: body),
                      try ControlFrame(command: .exchangeChanges, response: true, requestID: 7,
                          payload: Data([0]) + Data(repeating: 9, count: 16)),
                      try ControlFrame(command: .exchangeChanges, response: true, requestID: 7,
                          payload: Data([TransferResult.unauthorized.rawValue]) + consent.transaction)] {
            XCTAssertThrowsError(try CourseSwitchReply.validate(reply, request: request))
        }
    }

}

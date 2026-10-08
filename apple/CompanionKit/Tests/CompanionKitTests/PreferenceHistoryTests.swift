import Foundation
import XCTest
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif
@testable import CompanionKit

final class PreferenceHistoryTests: XCTestCase, @unchecked Sendable {
    func testSharedReaderBoundaryVectors() throws {
        var root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        for _ in 0 ..< 4 { root.deleteLastPathComponent() }
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf: root.appendingPathComponent("protocol/fixtures/PreferenceBodies-v1.json"))) as! [String: Any]
        let hex = Array((json["binaryHex"] as! String).utf8)
        var bytes = Data(); bytes.reserveCapacity(hex.count / 2)
        for at in stride(from: 0, to: hex.count, by: 2) {
            bytes.append(try XCTUnwrap(UInt8(String(decoding: hex[at ..< at + 2], as: UTF8.self), radix: 16)))
        }
        var reader = ByteReader(bytes)
        let count = try reader.number(2)
        for index in 0 ..< count {
            let accepted = try reader.number(1) == 1
            let length = Int(try reader.number(2))
            let body = try reader.take(length)
            if accepted {
                XCTAssertEqual(try PreferenceBody(decoding: body).encoded, body, "vector \(index)")
            } else {
                XCTAssertThrowsError(try PreferenceBody(decoding: body), "vector \(index)")
            }
        }
        XCTAssertEqual(reader.position, bytes.count)
    }
    private func mutation(_ body: PreferenceBody, origin: UInt8, sequence: UInt64 = 1, ancestors: [EventIdentity] = []) throws -> JournalMutation {
        var bytes = Data([1, 3]) + Data(repeating: origin, count: 16)
        bytes.appendLittleEndian(1, count: 8); bytes.appendLittleEndian(sequence, count: 8)
        bytes.append(Data(repeating: 2, count: 16)); bytes.append(4); bytes.append(PreferenceBody.scope)
        bytes.append(Data(SHA256.hash(data: body.encoded))); bytes.appendLittleEndian(1, count: 4)
        bytes.appendLittleEndian(0, count: 8); bytes.append(0); bytes.appendLittleEndian(0, count: 4)
        bytes.append(Data(count: 32)); bytes.append(UInt8(ancestors.count))
        for ancestor in ancestors { bytes.append(ancestor.storageKey) }
        return try JournalMutation(event: SyncEvent(decoding: bytes), body: body.encoded)
    }
    func testAllowlistRangesAndTypedBodies() throws {
        let bodies = [try PreferenceBody(key: .characterSpacing, value: .integer(-2)),
                      try PreferenceBody(key: .wordSpacing, value: .integer(150)),
                      try PreferenceBody(key: .language, value: .languageTag("zh-Hant-TW")),
                      try PreferenceBody(key: .dictionary, value: .content(nil))]
        for body in bodies {
            XCTAssertEqual(try PreferenceBody(decoding: body.encoded), body)
            for count in 0 ..< body.encoded.count { XCTAssertThrowsError(try PreferenceBody(decoding: body.encoded.prefix(count))) }
            XCTAssertThrowsError(try PreferenceBody(decoding: body.encoded + Data([0])))
        }
        for (key, value) in [(PreferenceKey.wordSpacing, Int32(51)), (.margin, 4), (.tintaRetentionPermille, 699), (.tintaMaximumInterval, 36501)] {
            XCTAssertThrowsError(try PreferenceBody(key: key, value: .integer(value)))
        }
        XCTAssertThrowsError(try PreferenceBody(key: .language, value: .languageTag("en/US")))
        var unknown = bodies[0].encoded; unknown[2] = 255
        XCTAssertThrowsError(try PreferenceBody(decoding: unknown))
        XCTAssertThrowsError(try PreferenceBody(key: .fontSelection, value: .content(nil)))
    }
    func testContentDependencyIsRetainedUntilInstalled() throws {
        let selection = try ContentSelection(hash: Data(repeating: 9, count: 32), name: "Noto Serif")
        let body = try PreferenceBody(key: .fontSelection, value: .content(selection))
        XCTAssertEqual(try PreferenceBody(decoding: body.encoded), body)
        let event = try mutation(body, origin: 1)
        let state = try XCTUnwrap(PreferenceHistory.reconcile([event]).first)
        XCTAssertEqual(state.missingContent(installed: []), selection)
        XCTAssertNil(state.missingContent(installed: [selection.hash]))
        XCTAssertThrowsError(try ContentSelection(hash: selection.hash, name: "../bad"))
    }
    func testConcurrentConflictAndResolutionAreCausal() throws {
        let first = try mutation(PreferenceBody(key: .margin, value: .integer(5)), origin: 1)
        let second = try mutation(PreferenceBody(key: .margin, value: .integer(10)), origin: 2)
        let conflict = try XCTUnwrap(PreferenceHistory.reconcile([second, first, second]).first)
        XCTAssertTrue(conflict.requiresResolution)
        let resolution = try mutation(PreferenceBody(key: .margin, value: .integer(10)), origin: 3, ancestors: conflict.resolutionAncestors)
        let resolved = try XCTUnwrap(PreferenceHistory.reconcile([first, second, resolution]).first)
        XCTAssertFalse(resolved.requiresResolution); XCTAssertEqual(resolved.candidates.count, 1)
        XCTAssertEqual(resolved.candidates.first?.body.value, .integer(10))
    }
    func testSQLiteReopenPreservesPreferenceConflict() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("library.sqlite")
        let first = try mutation(PreferenceBody(key: .alignment, value: .integer(0)), origin: 1)
        let second = try mutation(PreferenceBody(key: .alignment, value: .integer(1)), origin: 2)
        let store = try LibraryStore(url: url); try await store.importEvents([first, second])
        let reopened = try LibraryStore(url: url)
        let states = try await reopened.preferences()
        XCTAssertTrue(try XCTUnwrap(states.first).requiresResolution)
    }
}

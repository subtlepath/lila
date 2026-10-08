import Foundation
import XCTest
@testable import CompanionKit

final class WifiMessageCipherTests: XCTestCase {
    private let key = Data(0..<32)
    private let session = Data(repeating: 7, count: 16)

    func testDirectionsReplayAndAuthenticationRecovery() async throws {
        let apple = try WifiMessageCipher(key: key, session: session, sending: .appleToReader)
        let reader = try WifiMessageCipher(key: key, session: session, sending: .readerToApple)
        let payload = Data("control frame".utf8)
        let message = try await apple.seal(payload)
        var workspace = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { workspace.deleteLastPathComponent() }
        let fixture = try JSONSerialization.jsonObject(with: Data(contentsOf:
            workspace.appendingPathComponent("protocol/fixtures/WifiMessage.json"))) as? [String: Any]
        XCTAssertEqual(message.map { String(format: "%02x", $0) }.joined(), fixture?["binaryHex"] as? String)
        for offset in message.indices {
            var damaged = message
            damaged[offset] ^= 1
            do { _ = try await reader.open(damaged); XCTFail("tampering must fail") }
            catch { XCTAssertEqual(error as? WifiCipherError, offset < 31 ? .invalidMessage : .authentication) }
        }
        let opened = try await reader.open(message)
        XCTAssertEqual(opened, payload)
        do { _ = try await reader.open(message); XCTFail("replay must fail") }
        catch { XCTAssertEqual(error as? WifiCipherError, .invalidMessage) }
        do { _ = try await apple.open(message); XCTFail("reflection must fail") }
        catch { XCTAssertEqual(error as? WifiCipherError, .invalidMessage) }
        let reply = try await reader.seal(payload)
        XCTAssertNotEqual(reply, message)
        let response = try await apple.open(reply)
        XCTAssertEqual(response, payload)
    }

    func testBoundsAndSessionIsolation() async throws {
        let apple = try WifiMessageCipher(key: key, session: session, sending: .appleToReader)
        for payload in [Data(), Data(repeating: 1, count: WifiMessageCipher.maximumPayload + 1)] {
            do { _ = try await apple.seal(payload); XCTFail("oversized or empty message") }
            catch { XCTAssertEqual(error as? WifiCipherError, .invalidMessage) }
        }
        let payload = Data(repeating: 1, count: WifiMessageCipher.maximumPayload)
        let message = try await apple.seal(payload)
        let reader = try WifiMessageCipher(key: key, session: session, sending: .readerToApple)
        let opened = try await reader.open(message)
        XCTAssertEqual(opened, payload)
        let other = try WifiMessageCipher(key: key, session: Data(repeating: 8, count: 16), sending: .readerToApple)
        do { _ = try await other.open(message); XCTFail("different session") }
        catch { XCTAssertEqual(error as? WifiCipherError, .invalidMessage) }
        for count in [0, 31, 33] {
            XCTAssertThrowsError(try WifiMessageCipher(key: Data(repeating: 1, count: count), session: session, sending: .appleToReader))
        }
        await apple.invalidate()
        do { _ = try await apple.seal(payload); XCTFail("ended handoff cannot send") }
        catch { XCTAssertEqual(error as? WifiCipherError, .invalidSession) }
        await reader.invalidate()
        do { _ = try await reader.open(message); XCTFail("ended handoff cannot receive") }
        catch { XCTAssertEqual(error as? WifiCipherError, .invalidSession) }
    }
}

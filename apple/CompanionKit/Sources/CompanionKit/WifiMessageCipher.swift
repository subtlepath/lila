import Foundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif

public enum WifiCipherError: Error, Equatable, Sendable {
    case invalidSession, invalidMessage, exhausted, authentication, cryptoError
}

public enum WifiMessageDirection: UInt8, Sendable {
    case appleToReader = 0, readerToApple = 1
    fileprivate var opposite: Self { self == .appleToReader ? .readerToApple : .appleToReader }
}

// Each handoff requires a fresh key delivered over authenticated BLE.
public actor WifiMessageCipher {
    public static let maximumPayload = 1036
    private static let headerSize = 31
    private static let tagSize = 16
    private var key: SymmetricKey?
    private let session: Data
    private let sending: WifiMessageDirection
    private var sendCounter: UInt64 = 1
    private var receiveCounter: UInt64 = 1
    private var sendExhausted = false
    private var receiveExhausted = false

    public init(key: Data, session: Data, sending: WifiMessageDirection) throws {
        guard key.count == 32, session.count == 16, session.contains(where: { $0 != 0 }) else {
            throw WifiCipherError.invalidSession
        }
        self.key = SymmetricKey(data: key)
        self.session = session
        self.sending = sending
    }

    public func seal(_ payload: Data) throws -> Data {
        guard let key else { throw WifiCipherError.invalidSession }
        guard !sendExhausted else { throw WifiCipherError.exhausted }
        guard !payload.isEmpty, payload.count <= Self.maximumPayload else { throw WifiCipherError.invalidMessage }
        let counter = sendCounter
        // Consume before encryption so even a failed operation cannot reuse its nonce.
        if counter == UInt64.max { sendExhausted = true } else { sendCounter += 1 }
        let header = header(direction: sending, counter: counter, length: payload.count)
        do {
            let sealed = try AES.GCM.seal(payload, using: key, nonce: nonce(direction: sending, counter: counter),
                                          authenticating: header)
            return header + sealed.ciphertext + sealed.tag
        } catch {
            self.key = nil
            throw WifiCipherError.cryptoError
        }
    }

    public func open(_ message: Data) throws -> Data {
        guard let key else { throw WifiCipherError.invalidSession }
        guard !receiveExhausted else { throw WifiCipherError.exhausted }
        guard message.count >= Self.headerSize + Self.tagSize + 1,
              message.count <= Self.headerSize + Self.maximumPayload + Self.tagSize else {
            throw WifiCipherError.invalidMessage
        }
        let length = message.count - Self.headerSize - Self.tagSize
        let header = header(direction: sending.opposite, counter: receiveCounter, length: length)
        guard message.prefix(Self.headerSize) == header else { throw WifiCipherError.invalidMessage }
        let ciphertext = message.dropFirst(Self.headerSize).dropLast(Self.tagSize)
        let plaintext: Data
        do {
            let box = try AES.GCM.SealedBox(nonce: nonce(direction: sending.opposite, counter: receiveCounter),
                                            ciphertext: ciphertext, tag: message.suffix(Self.tagSize))
            plaintext = try AES.GCM.open(box, using: key, authenticating: header)
        } catch { throw WifiCipherError.authentication }
        if receiveCounter == UInt64.max { receiveExhausted = true } else { receiveCounter += 1 }
        return plaintext
    }

    public func invalidate() { key = nil }

    private func header(direction: WifiMessageDirection, counter: UInt64, length: Int) -> Data {
        var bytes = Data([0x4c, 0x57, 0x48, 1])
        bytes.append(session)
        bytes.append(direction.rawValue)
        bytes.appendLittleEndian(counter, count: 8)
        bytes.appendLittleEndian(UInt64(length), count: 2)
        return bytes
    }

    private func nonce(direction: WifiMessageDirection, counter: UInt64) throws -> AES.GCM.Nonce {
        var bytes = Data([0x4c, 0x57, 0x48, direction.rawValue])
        bytes.appendLittleEndian(counter, count: 8)
        return try AES.GCM.Nonce(data: bytes)
    }
}

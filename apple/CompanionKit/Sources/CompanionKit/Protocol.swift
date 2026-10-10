import Foundation

public enum ProtocolError: Error, Equatable, Sendable {
    case truncated, magic, version, command, flags, length, unauthorized, record, value
}

public enum Command: UInt8, CaseIterable, Sendable {
    case discover = 1, inventory, exchangeChanges, beginTransfer, transferChunk
    case transferStatus, commit, abort, wifiHandoff, installFirmware, error
    case registerInstallation, authenticateInstallation, journalFormats, removeContent, readContent, contentMetadata, prepareContentHandoff, courseContext
    case courseBaselineReview
}

public struct ControlFrame: Equatable, Sendable {
    public static let headerSize = 12
    public static let maximumPayload = 1024
    public let command: Command
    public let response: Bool
    public let requestID: UInt32
    public let payload: Data

    public init(command: Command, response: Bool = false, requestID: UInt32, payload: Data = Data()) throws {
        guard payload.count <= Self.maximumPayload else { throw ProtocolError.length }
        self.command = command
        self.response = response
        self.requestID = requestID
        self.payload = payload
    }

    public init(decoding data: Data, authenticated: Bool) throws {
        guard data.count >= Self.headerSize else { throw ProtocolError.truncated }
        var reader = ByteReader(data)
        guard try reader.number(2) == 0x434c else { throw ProtocolError.magic }
        guard try reader.number(1) == 1 else { throw ProtocolError.version }
        guard let command = Command(rawValue: UInt8(try reader.number(1))) else { throw ProtocolError.command }
        let flags = try reader.number(1)
        guard flags <= 1, try reader.number(1) == 0 else { throw ProtocolError.flags }
        let requestID = UInt32(try reader.number(4))
        let length = Int(try reader.number(2))
        guard length <= Self.maximumPayload, data.count == Self.headerSize + length else { throw ProtocolError.length }
        guard authenticated || command == .discover else { throw ProtocolError.unauthorized }
        self.command = command
        response = flags == 1
        self.requestID = requestID
        payload = try reader.take(length)
    }

    public func encoded() -> Data {
        var bytes = Data([0x4c, 0x43, 1, command.rawValue, response ? 1 : 0, 0])
        bytes.reserveCapacity(Self.headerSize + payload.count)
        bytes.appendLittleEndian(UInt64(requestID), count: 4)
        bytes.appendLittleEndian(UInt64(payload.count), count: 2)
        bytes.append(payload)
        return bytes
    }
}

public struct FrameAssembler: Sendable {
    private var bytes = Data()
    public init() { bytes.reserveCapacity(ControlFrame.headerSize + ControlFrame.maximumPayload) }
    public mutating func reset() { bytes.removeAll(keepingCapacity: true) }

    public mutating func append(_ fragment: Data, authenticated: Bool) throws -> ControlFrame? {
        guard !fragment.isEmpty,
              bytes.count + fragment.count <= ControlFrame.headerSize + ControlFrame.maximumPayload else {
            reset()
            throw ProtocolError.length
        }
        bytes.append(fragment)
        guard bytes.count >= ControlFrame.headerSize else { return nil }
        let start = bytes.startIndex
        let length = Int(bytes[start + 10]) | (Int(bytes[start + 11]) << 8)
        guard length <= ControlFrame.maximumPayload, bytes.count <= ControlFrame.headerSize + length else {
            reset()
            throw ProtocolError.length
        }
        guard bytes[start] == 0x4c, bytes[start + 1] == 0x43 else { reset(); throw ProtocolError.magic }
        guard bytes[start + 2] == 1 else { reset(); throw ProtocolError.version }
        guard Command(rawValue: bytes[start + 3]) != nil else { reset(); throw ProtocolError.command }
        guard bytes[start + 4] <= 1, bytes[start + 5] == 0 else { reset(); throw ProtocolError.flags }
        guard authenticated || bytes[start + 3] == Command.discover.rawValue else { reset(); throw ProtocolError.unauthorized }
        guard bytes.count == ControlFrame.headerSize + length else { return nil }
        defer { reset() }
        return try ControlFrame(decoding: bytes, authenticated: authenticated)
    }
}

struct ByteReader {
    let bytes: Data
    var position = 0
    init(_ bytes: Data) { self.bytes = bytes }
    mutating func take(_ count: Int) throws -> Data {
        guard count >= 0, count <= bytes.count - position else { throw ProtocolError.truncated }
        let start = bytes.startIndex + position
        position += count
        return Data(bytes[start ..< start + count])
    }
    mutating func number(_ count: Int) throws -> UInt64 {
        guard (1 ... 8).contains(count), count <= bytes.count - position else { throw ProtocolError.truncated }
        var result: UInt64 = 0
        for offset in 0 ..< count { result |= UInt64(bytes[bytes.startIndex + position + offset]) << (8 * offset) }
        position += count
        return result
    }
}

extension Data {
    mutating func appendLittleEndian(_ number: UInt64, count: Int) {
        for index in 0 ..< count { append(UInt8(truncatingIfNeeded: number >> (8 * index))) }
    }
}

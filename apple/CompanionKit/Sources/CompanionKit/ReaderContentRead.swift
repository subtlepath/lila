import Foundation

public struct ReaderContentReadRequest: Equatable, Sendable {
    public static let encodedSize = 93
    public static let maximumChunkBytes = ControlFrame.maximumPayload - ReaderContentReadReply.headerSize
    public let generation: Data
    public let manifest: ContentManifest
    public let offset: UInt64
    public let maximumBytes: UInt16

    public init(generation: Data, manifest: ContentManifest, offset: UInt64, maximumBytes: UInt16) throws {
        guard generation.count == 16, generation.contains(where: { $0 != 0 }),
              manifest.length > 0, manifest.content.digest.contains(where: { $0 != 0 }),
              offset < manifest.length, maximumBytes > 0,
              Int(maximumBytes) <= Self.maximumChunkBytes else { throw ProtocolError.value }
        let family = manifest.logicalIdentity.contains(where: { $0 != 0 })
        let compatible: Bool
        switch manifest.kind {
        case .epub: compatible = manifest.formatVersion <= 1 && !family
        case .course: compatible = manifest.formatVersion == 1 && family
        case .font: compatible = [1, 4].contains(manifest.formatVersion) && !family
        case .dictionary: compatible = manifest.formatVersion == 1 && !family
        case .firmware: compatible = false
        }
        guard compatible else { throw ProtocolError.value }
        self.generation = generation; self.manifest = manifest
        self.offset = offset; self.maximumBytes = maximumBytes
    }
    public var encoded: Data {
        var bytes = Data([0x4c, 0x43, 0x52, 1]); bytes.reserveCapacity(Self.encodedSize)
        bytes.append(generation); bytes.append(manifest.encoded)
        bytes.appendLittleEndian(offset, count: 8)
        bytes.appendLittleEndian(UInt64(maximumBytes), count: 2)
        return bytes
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x4c, 0x43, 0x52, 1]) else { throw ProtocolError.version }
        try self.init(generation: reader.take(16), manifest: ContentManifest(decoding: reader.take(63)),
                      offset: reader.number(8), maximumBytes: UInt16(reader.number(2)))
    }
}

public enum ReaderContentReadResult: UInt8, CaseIterable, Sendable {
    case ok, invalid, unauthorized, wrongStorage, busy, notFound, corrupt, ioError
}

public struct ReaderContentReadReply: Equatable, Sendable {
    public static let headerSize = 63
    public let result: ReaderContentReadResult
    public let bytes: Data
    public init(decoding data: Data, request: ReaderContentReadRequest) throws {
        guard data.count >= Self.headerSize, data.count <= ControlFrame.maximumPayload else { throw ProtocolError.length }
        var reader = ByteReader(data)
        guard try reader.take(4) == Data([0x4c, 0x43, 0x53, 1]) else { throw ProtocolError.version }
        guard let result = ReaderContentReadResult(rawValue: UInt8(try reader.number(1))) else { throw ProtocolError.value }
        guard try reader.take(16) == request.generation,
              try reader.take(32) == request.manifest.content.digest,
              try reader.number(8) == request.offset else { throw ProtocolError.value }
        let count = Int(try reader.number(2))
        guard data.count == Self.headerSize + count else { throw ProtocolError.length }
        if result == .ok {
            guard UInt64(count) == min(UInt64(request.maximumBytes), request.manifest.length - request.offset) else {
                throw ProtocolError.value
            }
        } else if count != 0 { throw ProtocolError.value }
        self.result = result; bytes = try reader.take(count)
    }
}

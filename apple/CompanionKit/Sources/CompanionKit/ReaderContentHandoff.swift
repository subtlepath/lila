import Foundation

public struct ReaderContentHandoffRequest: Equatable, Sendable {
    public static let encodedSize = 107
    public static let wifiThreshold: UInt64 = 1024 * 1024
    public let transaction: Data
    public let read: ReaderContentReadRequest

    public init(transaction: Data, generation: Data, manifest: ContentManifest, offset: UInt64) throws {
        guard transaction.count == 16, transaction.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        read = try ReaderContentReadRequest(generation: generation, manifest: manifest, offset: offset, maximumBytes: 1)
        guard manifest.length - offset > Self.wifiThreshold else { throw ProtocolError.value }
        self.transaction = transaction
    }
    public var encoded: Data {
        var bytes = Data([0x4c, 0x43, 0x57, 1]); bytes.reserveCapacity(Self.encodedSize)
        bytes.append(transaction); bytes.append(read.generation); bytes.append(read.manifest.encoded)
        bytes.appendLittleEndian(read.offset, count: 8)
        return bytes
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x4c, 0x43, 0x57, 1]) else { throw ProtocolError.version }
        try self.init(transaction: reader.take(16), generation: reader.take(16),
                      manifest: ContentManifest(decoding: reader.take(63)), offset: reader.number(8))
    }
}

public struct ReaderContentHandoffReply: Equatable, Sendable {
    public static let encodedSize = 77
    public let result: ReaderContentReadResult
    public init(decoding bytes: Data, request: ReaderContentHandoffRequest) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x4c, 0x43, 0x54, 1]) else { throw ProtocolError.version }
        guard let result = ReaderContentReadResult(rawValue: UInt8(try reader.number(1))),
              try reader.take(16) == request.transaction,
              try reader.take(16) == request.read.generation,
              try reader.take(32) == request.read.manifest.content.digest,
              try reader.number(8) == request.read.offset else { throw ProtocolError.value }
        self.result = result
    }
}

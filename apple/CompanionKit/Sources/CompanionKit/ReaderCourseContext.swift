import Foundation

public struct ReaderCourseContextRequest: Equatable, Sendable {
    public static let encodedSize = 20
    public let generation: Data
    public init(generation: Data) throws {
        guard generation.count == 16, generation.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        self.generation = generation
    }
    public var encoded: Data {
        var bytes = Data([0x4c, 0x43, 0x51, 1]); bytes.reserveCapacity(Self.encodedSize)
        bytes.append(generation); return bytes
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        guard bytes.prefix(4) == Data([0x4c, 0x43, 0x51, 1]) else { throw ProtocolError.version }
        try self.init(generation: Data(bytes.dropFirst(4)))
    }
}
public enum ReaderCourseContextResult: UInt8, CaseIterable, Sendable {
    case ok, missing, wrongStorage, busy, unsupported, ioError, unauthorized, corrupt
}
public enum ReaderCourseContextSource: UInt8, Sendable { case live = 1, removed = 2 }
public struct ReaderCourseContext: Equatable, Sendable {
    public let generation: Data
    public let source: ReaderCourseContextSource
    public let manifest: ContentManifest
    public init(generation: Data, source: ReaderCourseContextSource, manifest: ContentManifest) throws {
        _ = try ReaderCourseContextRequest(generation: generation)
        guard manifest.kind == .course, manifest.length > 0, manifest.formatVersion == 1,
              manifest.logicalIdentity.contains(where: { $0 != 0 }),
              manifest.content.digest.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        self.generation = generation; self.source = source; self.manifest = manifest
    }
}
public struct ReaderCourseContextReply: Equatable, Sendable {
    public static let headerSize = 22
    public static let maximumSize = 85
    public let result: ReaderCourseContextResult
    public let generation: Data
    public let context: ReaderCourseContext?
    public init(decoding bytes: Data, request: ReaderCourseContextRequest) throws {
        guard bytes.count >= Self.headerSize else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x4c, 0x43, 0x58, 1]) else { throw ProtocolError.version }
        guard let result = ReaderCourseContextResult(rawValue: UInt8(try reader.number(1))) else { throw ProtocolError.value }
        let source = UInt8(try reader.number(1)), generation = try reader.take(16)
        _ = try ReaderCourseContextRequest(generation: generation)
        guard (result == .wrongStorage) != (generation == request.generation) else { throw ProtocolError.value }
        if result == .ok {
            guard bytes.count == Self.maximumSize else { throw ProtocolError.length }
            guard let source = ReaderCourseContextSource(rawValue: source) else { throw ProtocolError.value }
            context = try ReaderCourseContext(generation: generation, source: source,
                                               manifest: ContentManifest(decoding: reader.take(63)))
        } else {
            guard bytes.count == Self.headerSize, source == 0 else { throw ProtocolError.value }
            context = nil
        }
        self.result = result; self.generation = generation
    }
}

import Foundation

public struct ReaderContentMetadataRequest: Equatable, Sendable {
    public static let encodedSize = 83
    public let generation: Data
    public let manifest: ContentManifest

    public init(generation: Data, manifest: ContentManifest) throws {
        _ = try ReaderContentReadRequest(generation: generation, manifest: manifest, offset: 0, maximumBytes: 1)
        self.generation = generation
        self.manifest = manifest
    }

    public var encoded: Data {
        var bytes = Data([0x4c, 0x43, 0x4d, 1])
        bytes.reserveCapacity(Self.encodedSize)
        bytes.append(generation)
        bytes.append(manifest.encoded)
        return bytes
    }

    public init(decoding bytes: Data) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x4c, 0x43, 0x4d, 1]) else { throw ProtocolError.version }
        try self.init(generation: reader.take(16), manifest: ContentManifest(decoding: reader.take(63)))
    }
}

public struct ReaderContentMetadataReply: Equatable, Sendable {
    public static let headerSize = 85
    public let result: ReaderContentReadResult
    public let originalFilename: String?
    public let generation: Data
    public let manifest: ContentManifest

    public init(decoding bytes: Data, request: ReaderContentMetadataRequest) throws {
        guard bytes.count >= Self.headerSize, bytes.count <= Self.headerSize + 255 else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x4c, 0x43, 0x4e, 1]) else { throw ProtocolError.version }
        guard let result = ReaderContentReadResult(rawValue: UInt8(try reader.number(1))),
              try reader.take(16) == request.generation,
              try reader.take(63) == request.manifest.encoded else { throw ProtocolError.value }
        let length = Int(try reader.number(1))
        guard bytes.count == Self.headerSize + length else { throw ProtocolError.length }
        if result == .ok {
            guard let name = String(data: try reader.take(length), encoding: .utf8),
                  name != ".", name != "..",
                  !name.unicodeScalars.contains(where: { $0.value < 32 || $0.value == 127 }) else {
                throw ProtocolError.value
            }
            let content = LibraryContent(id: request.manifest.content, kind: request.manifest.kind,
                                         length: request.manifest.length, title: name, originalFilename: name)
            try CloudContentDescriptor(content: content).validate()
            let suffix = (name as NSString).pathExtension.lowercased()
            if request.manifest.kind == .font {
                guard (suffix == "cpfont") == (request.manifest.formatVersion == 4) else { throw ProtocolError.value }
            }
            originalFilename = name
        } else {
            guard length == 0 else { throw ProtocolError.value }
            originalFilename = nil
        }
        self.result = result
        generation = request.generation
        manifest = request.manifest
    }
}

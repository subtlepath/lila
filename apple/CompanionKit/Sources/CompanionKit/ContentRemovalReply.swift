import Foundation

public enum ContentRemovalResult: UInt8, Sendable, CaseIterable {
    case ok, invalid, unauthorized, wrongStorage, busy, notFound, unsupported, conflict, corrupt, ioError
}

/// Authorized request replies echo their transaction identity on every outcome.
public struct ContentRemovalReply: Equatable, Sendable {
    public static let encodedSize = 17
    public let result: ContentRemovalResult
    public let transaction: Data

    public init(decoding bytes: Data, request: ContentRemovalRequest) throws {
        guard bytes.count == Self.encodedSize else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard let result = ContentRemovalResult(rawValue: UInt8(try reader.number(1))) else {
            throw ProtocolError.value
        }
        let transaction = try reader.take(16)
        // Installation authorization fails before request parsing on the reader.
        if result == .unauthorized && transaction == Data(repeating: 0, count: 16) {
            throw ProtocolError.unauthorized
        }
        guard transaction == request.transaction else { throw ProtocolError.value }
        self.result = result
        self.transaction = transaction
    }
}

import Foundation

public enum InventoryError: Error, Equatable, Sendable { case changedSnapshot, wrongCursor, duplicateContent, exhausted, incomplete }
public struct InventoryPage: Sendable {
    public static let maximumEntries = 8
    public let generation: Data
    public let revision: UInt64
    public let cursor: UInt64
    public let nextCursor: UInt64
    public let complete: Bool
    public let contents: [ContentManifest]
    public init(decoding bytes: Data) throws {
        guard bytes.count >= 43 else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.number(1) == 1 else { throw ProtocolError.version }
        generation = try reader.take(16); revision = try reader.number(8)
        cursor = try reader.number(8); nextCursor = try reader.number(8)
        let terminal = try reader.number(1), count = Int(try reader.number(1))
        guard generation.contains(where: { $0 != 0 }), revision > 0, terminal <= 1,
              count <= Self.maximumEntries, cursor <= UInt64.max - UInt64(count), bytes.count == 43 + count * 63 else { throw ProtocolError.value }
        complete = terminal == 1
        if complete { guard nextCursor == 0 else { throw InventoryError.wrongCursor } }
        else {
            guard count > 0, cursor <= UInt64.max - UInt64(count), nextCursor == cursor + UInt64(count) else {
                throw InventoryError.wrongCursor
            }
        }
        var contents: [ContentManifest] = []; contents.reserveCapacity(count)
        var seen = Set<ContentID>(); seen.reserveCapacity(count)
        for _ in 0..<count {
            let manifest = try ContentManifest(decoding: reader.take(63))
            guard seen.insert(manifest.content).inserted else { throw InventoryError.duplicateContent }
            contents.append(manifest)
        }
        self.contents = contents
    }
    public static func request(generation: Data, revision: UInt64, cursor: UInt64, requestID: UInt32,
                               limit: UInt8 = 8) throws -> ControlFrame {
        guard generation.count == 16, generation.contains(where: { $0 != 0 }), (1...8).contains(limit),
              revision > 0 || cursor == 0 else { throw ProtocolError.value }
        var bytes = Data([1]); bytes.reserveCapacity(34); bytes.append(generation)
        bytes.appendLittleEndian(revision, count: 8); bytes.appendLittleEndian(cursor, count: 8); bytes.append(limit)
        return try ControlFrame(command: .inventory, requestID: requestID, payload: bytes)
    }
}
public struct InventoryScan: Sendable {
    private let reader: Data
    private let generation: Data
    private let maximumEntries: Int
    private var revision: UInt64?
    public private(set) var cursor: UInt64 = 0
    public private(set) var complete = false
    private var contents: [ContentManifest] = []
    private var seen = Set<ContentID>()
    public init(reader: Data, generation: Data, maximumEntries: Int) throws {
        guard [reader, generation].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }),
              maximumEntries >= 0 else { throw ProtocolError.value }
        self.reader = reader; self.generation = generation; self.maximumEntries = maximumEntries
        contents.reserveCapacity(min(maximumEntries, 256)); seen.reserveCapacity(min(maximumEntries, 256))
    }
    public mutating func append(_ page: InventoryPage) throws {
        guard !complete else { throw InventoryError.exhausted }
        guard page.generation == generation, revision == nil || page.revision == revision else { throw InventoryError.changedSnapshot }
        guard page.cursor == cursor else { throw InventoryError.wrongCursor }
        guard page.contents.count <= maximumEntries - contents.count else { throw InventoryError.exhausted }
        for entry in page.contents where seen.contains(entry.content) { throw InventoryError.duplicateContent }
        revision = page.revision
        for entry in page.contents { seen.insert(entry.content); contents.append(entry) }
        cursor = page.nextCursor; complete = page.complete
    }
    public func nextRequest(requestID: UInt32) throws -> ControlFrame {
        guard !complete else { throw InventoryError.exhausted }
        return try InventoryPage.request(generation: generation, revision: revision ?? 0, cursor: cursor, requestID: requestID)
    }
    public func inventory() throws -> ReaderInventory {
        guard complete else { throw InventoryError.incomplete }
        return try ReaderInventory(reader: reader, generation: generation, contents: contents, complete: true)
    }
}

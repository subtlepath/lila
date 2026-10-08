import Foundation

public struct BookmarkValue: Equatable, Sendable {
    public let anchor: ReadingAnchor
    public let name: String
    public let summary: String
    public init(anchor: ReadingAnchor, name: String = "", summary: String = "") throws {
        guard name.utf8.count <= 128, summary.utf8.count <= 512,
              !name.utf8.contains(0), !summary.utf8.contains(0) else { throw ProtocolError.value }
        self.anchor = anchor; self.name = name; self.summary = summary
    }
}
public struct BookmarkBody: Equatable, Sendable {
    public let identity: Data
    // Nil is an explicit deletion tombstone.
    public let value: BookmarkValue?
    public init(identity: Data, value: BookmarkValue?) throws {
        guard identity.count == 16, identity.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        self.identity = identity; self.value = value
    }
    public var kind: SyncEventKind { value == nil ? .bookmarkDelete : .bookmarkPut }
    public var encoded: Data {
        var bytes = Data([1, kind.rawValue]); bytes.reserveCapacity(668)
        bytes.append(identity)
        if let value {
            bytes.append(value.anchor.encoded.dropFirst(2))
            bytes.appendLittleEndian(UInt64(value.name.utf8.count), count: 2); bytes.append(contentsOf: value.name.utf8)
            bytes.appendLittleEndian(UInt64(value.summary.utf8.count), count: 2); bytes.append(contentsOf: value.summary.utf8)
        }
        return bytes
    }
    public init(decoding bytes: Data) throws {
        guard (18 ... 668).contains(bytes.count) else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.number(1) == 1 else { throw ProtocolError.version }
        let kind = try reader.number(1)
        guard kind == UInt64(SyncEventKind.bookmarkPut.rawValue) || kind == UInt64(SyncEventKind.bookmarkDelete.rawValue) else { throw ProtocolError.value }
        let identity = try reader.take(16)
        var value: BookmarkValue?
        if kind == UInt64(SyncEventKind.bookmarkPut.rawValue) {
            let anchor = try ReadingAnchor(spine: UInt16(reader.number(2)), visibleTextOffset: UInt32(reader.number(4)))
            let nameLength = Int(try reader.number(2)); guard nameLength <= 128 else { throw ProtocolError.length }
            let nameBytes = try reader.take(nameLength)
            let summaryLength = Int(try reader.number(2)); guard summaryLength <= 512 else { throw ProtocolError.length }
            guard let name = String(data: nameBytes, encoding: .utf8),
                  let summary = String(data: try reader.take(summaryLength), encoding: .utf8) else { throw ProtocolError.value }
            value = try BookmarkValue(anchor: anchor, name: name, summary: summary)
        }
        guard reader.position == bytes.count else { throw ProtocolError.length }
        try self.init(identity: identity, value: value)
    }
    public init(mutation: JournalMutation) throws {
        try self.init(decoding: mutation.body)
        guard mutation.event.kind == kind, mutation.event.resource.contains(where: { $0 != 0 }),
              mutation.event.schedulerVersion == 0, mutation.event.schedulerConfiguration == Data(count: 32) else { throw ProtocolError.value }
    }
}
public struct BookmarkCandidate: Equatable, Sendable {
    public let event: EventIdentity
    public let value: BookmarkValue?
}
public struct BookmarkState: Equatable, Sendable {
    public let identity: Data
    public let candidates: [BookmarkCandidate]
    public var isDeleted: Bool { !candidates.isEmpty && candidates.allSatisfy { $0.value == nil } }
    public var requiresResolution: Bool {
        guard let first = candidates.first else { return false }
        return candidates.contains { $0.value != first.value }
    }
    public var resolutionAncestors: [EventIdentity] { candidates.map(\.event) }
}
public enum BookmarkHistory {
    public static func reconcile(_ deliveries: [JournalMutation], content: Data) throws -> [BookmarkState] {
        guard content.count == 32, content.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        let history = try CausalHistory(events: deliveries.map(\.event))
        var groups: [Data: [EventIdentity: BookmarkCandidate]] = [:]; groups.reserveCapacity(64)
        for mutation in deliveries where mutation.event.resource == content &&
            (mutation.event.kind == .bookmarkPut || mutation.event.kind == .bookmarkDelete) {
            let body = try BookmarkBody(mutation: mutation)
            groups[body.identity, default: [:]][mutation.event.identity] = BookmarkCandidate(event: mutation.event.identity, value: body.value)
        }
        var states: [BookmarkState] = []; states.reserveCapacity(groups.count)
        for identity in groups.keys.sorted(by: { $0.lexicographicallyPrecedes($1) }) {
            guard let group = groups[identity] else { throw ProtocolError.record }
            let heads = try history.maximal(Array(group.keys))
            let candidates = try heads.map { event in
                guard let candidate = group[event] else { throw ProtocolError.record }
                return candidate
            }
            states.append(BookmarkState(identity: identity, candidates: candidates))
        }
        return states
    }
}

import Foundation

public struct CausalHistory: Sendable {
    private let parents: [EventIdentity: [EventIdentity]]
    public init(events: [SyncEvent]) throws {
        let ordered = try SyncHistory.merged(events)
        var parents: [EventIdentity: [EventIdentity]] = [:]; parents.reserveCapacity(ordered.count)
        for event in ordered {
            var dependencies = event.ancestors
            dependencies.reserveCapacity(5)
            if event.identity.sequence > 1 {
                let previous = try EventIdentity(origin: event.identity.origin, epoch: event.identity.epoch, sequence: event.identity.sequence - 1)
                if !dependencies.contains(previous) { dependencies.append(previous) }
            }
            parents[event.identity] = dependencies
        }
        self.parents = parents
    }
    // Return every candidate not causally superseded by another candidate.
    public func maximal(_ candidates: [EventIdentity]) throws -> [EventIdentity] {
        let identities = Set(candidates)
        var pending: [EventIdentity] = []; pending.reserveCapacity(parents.count)
        for identity in identities {
            guard let dependencies = parents[identity] else { throw HistoryError.missingAncestor(identity) }
            pending.append(contentsOf: dependencies)
        }
        var visited = Set<EventIdentity>(); visited.reserveCapacity(parents.count)
        while let identity = pending.popLast() {
            guard visited.insert(identity).inserted else { continue }
            guard let dependencies = parents[identity] else { throw HistoryError.missingAncestor(identity) }
            pending.append(contentsOf: dependencies)
        }
        return identities.subtracting(visited).sorted()
    }
}

public struct ReadingAnchor: Equatable, Sendable {
    public let spine: UInt16
    public let visibleTextOffset: UInt32
    public init(spine: UInt16, visibleTextOffset: UInt32) {
        self.spine = spine; self.visibleTextOffset = visibleTextOffset
    }
    public var encoded: Data {
        var bytes = Data([1, SyncEventKind.readingPosition.rawValue])
        bytes.appendLittleEndian(UInt64(spine), count: 2)
        bytes.appendLittleEndian(UInt64(visibleTextOffset), count: 4)
        return bytes
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == 8 else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.number(1) == 1 else { throw ProtocolError.version }
        guard try reader.number(1) == UInt64(SyncEventKind.readingPosition.rawValue) else { throw ProtocolError.value }
        spine = UInt16(try reader.number(2)); visibleTextOffset = UInt32(try reader.number(4))
    }
}
public struct ReadingPosition: Equatable, Sendable {
    public let identity: EventIdentity
    public let anchor: ReadingAnchor
}
public struct ReadingPositions: Equatable, Sendable {
    public let content: Data
    public let candidates: [ReadingPosition]
    public var requiresResolution: Bool {
        guard let first = candidates.first else { return false }
        return candidates.contains { $0.anchor != first.anchor }
    }
    // All causal heads are needed even when concurrent candidates have equal anchors.
    public var resolutionAncestors: [EventIdentity] { candidates.map(\.identity) }
}
public enum ReadingHistory {
    public static func reconcile(_ deliveries: [JournalMutation], content: Data) throws -> ReadingPositions {
        guard content.count == 32, content.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        let history = try CausalHistory(events: deliveries.map(\.event))
        var positions: [EventIdentity: ReadingPosition] = [:]; positions.reserveCapacity(32)
        for mutation in deliveries where mutation.event.kind == .readingPosition && mutation.event.resource == content {
            let event = mutation.event
            guard event.schedulerVersion == 0, event.schedulerConfiguration == Data(count: 32) else { throw ProtocolError.value }
            positions[event.identity] = ReadingPosition(identity: event.identity, anchor: try ReadingAnchor(decoding: mutation.body))
        }
        let heads = try history.maximal(Array(positions.keys))
        let candidates = try heads.map { identity in
            guard let position = positions[identity] else { throw ProtocolError.record }
            return position
        }
        return ReadingPositions(content: content, candidates: candidates)
    }
}

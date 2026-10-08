import Foundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif

public struct EventIdentity: Hashable, Sendable, Comparable {
    public let origin: Data
    public let epoch: UInt64
    public let sequence: UInt64
    public init(origin: Data, epoch: UInt64, sequence: UInt64) throws {
        guard origin.count == 16, origin.contains(where: { $0 != 0 }), epoch > 0, sequence > 0 else { throw ProtocolError.value }
        self.origin = origin; self.epoch = epoch; self.sequence = sequence
    }
    public static func < (lhs: Self, rhs: Self) -> Bool {
        if lhs.origin != rhs.origin { return lhs.origin.lexicographicallyPrecedes(rhs.origin) }
        if lhs.epoch != rhs.epoch { return lhs.epoch < rhs.epoch }
        return lhs.sequence < rhs.sequence
    }
    fileprivate static func decode(_ reader: inout ByteReader) throws -> Self {
        try Self(origin: reader.take(16), epoch: reader.number(8), sequence: reader.number(8))
    }
}

public enum SyncEventKind: UInt8, Sendable {
    case readingPosition = 1, bookmarkPut, bookmarkDelete, preference, review, undoReview
    case suspension, lessonComplete, star, readingComplete
}
public enum ClockQuality: UInt8, Sendable { case unknown, device, trusted }

public struct SyncEvent: Equatable, Sendable {
    public let identity: EventIdentity
    public let storageGeneration: Data
    public let kind: SyncEventKind
    public let resource: Data
    public let bodyHash: Data
    public let studyDay: UInt32
    public let timestamp: UInt64
    public let clockQuality: ClockQuality
    public let schedulerVersion: UInt32
    public let schedulerConfiguration: Data
    public let ancestors: [EventIdentity]
    public let bytes: Data
    public init(identity: EventIdentity, storageGeneration: Data, kind: SyncEventKind, resource: Data, bodyHash: Data,
                studyDay: UInt32 = 0, timestamp: UInt64 = 0, clockQuality: ClockQuality = .unknown,
                schedulerVersion: UInt32 = 0, schedulerConfiguration: Data = Data(count: 32), ancestors: [EventIdentity] = []) throws {
        guard storageGeneration.count == 16, resource.count == 32, bodyHash.count == 32,
              schedulerConfiguration.count == 32, ancestors.count <= 4 else { throw ProtocolError.length }
        var bytes = Data([1, RecordKind.syncEvent.rawValue]); bytes.reserveCapacity(165 + ancestors.count * 32)
        bytes.append(identity.storageKey); bytes.append(storageGeneration); bytes.append(kind.rawValue)
        bytes.append(resource); bytes.append(bodyHash); bytes.appendLittleEndian(UInt64(studyDay), count: 4)
        bytes.appendLittleEndian(timestamp, count: 8); bytes.append(clockQuality.rawValue)
        bytes.appendLittleEndian(UInt64(schedulerVersion), count: 4); bytes.append(schedulerConfiguration)
        bytes.append(UInt8(ancestors.count)); for ancestor in ancestors { bytes.append(ancestor.storageKey) }
        try self.init(decoding: bytes)
    }
    public init(decoding bytes: Data) throws {
        guard try RecordEnvelope(decoding: bytes).kind == .syncEvent else { throw ProtocolError.record }
        var reader = ByteReader(bytes)
        _ = try reader.take(2)
        identity = try EventIdentity.decode(&reader)
        storageGeneration = try reader.take(16)
        guard storageGeneration.contains(where: { $0 != 0 }), let kind = SyncEventKind(rawValue: UInt8(try reader.number(1))) else { throw ProtocolError.value }
        self.kind = kind
        resource = try reader.take(32); bodyHash = try reader.take(32)
        studyDay = UInt32(try reader.number(4)); timestamp = try reader.number(8)
        guard let quality = ClockQuality(rawValue: UInt8(try reader.number(1))) else { throw ProtocolError.value }
        clockQuality = quality
        schedulerVersion = UInt32(try reader.number(4)); schedulerConfiguration = try reader.take(32)
        let count = Int(try reader.number(1))
        var dependencies: [EventIdentity] = []; dependencies.reserveCapacity(count)
        for _ in 0 ..< count {
            let ancestor = try EventIdentity.decode(&reader)
            guard ancestor != identity, !dependencies.contains(ancestor),
                  ancestor.origin != identity.origin || ancestor.epoch != identity.epoch || ancestor.sequence < identity.sequence else { throw ProtocolError.value }
            dependencies.append(ancestor)
        }
        ancestors = dependencies; self.bytes = Data(bytes)
    }
}

public enum HistoryError: Error, Equatable, Sendable {
    case equivocation(EventIdentity), missingAncestor(EventIdentity), sequenceGap(EventIdentity), causalCycle, staleFrontier
}

public enum SyncHistory {
    // Complete journals start at sequence one. Partial pages must be assembled before replay.
    public static func merged(_ deliveries: [SyncEvent]) throws -> [SyncEvent] {
        var events: [EventIdentity: SyncEvent] = [:]; events.reserveCapacity(deliveries.count)
        for event in deliveries {
            if let existing = events[event.identity], existing.bytes != event.bytes { throw HistoryError.equivocation(event.identity) }
            events[event.identity] = event
        }
        var dependencies: [EventIdentity: Set<EventIdentity>] = [:]; dependencies.reserveCapacity(events.count)
        var successors: [EventIdentity: [EventIdentity]] = [:]; successors.reserveCapacity(events.count)
        for event in events.values {
            var parents = Set(event.ancestors)
            for parent in parents where events[parent] == nil { throw HistoryError.missingAncestor(parent) }
            if event.identity.sequence > 1 {
                let previous = try EventIdentity(origin: event.identity.origin, epoch: event.identity.epoch, sequence: event.identity.sequence - 1)
                guard events[previous] != nil else { throw HistoryError.sequenceGap(previous) }
                parents.insert(previous)
            }
            dependencies[event.identity] = parents
            for parent in parents { successors[parent, default: []].append(event.identity) }
        }
        var ready: [SyncEvent] = []; ready.reserveCapacity(events.count)
        for event in events.values where dependencies[event.identity]?.isEmpty == true { insert(event, into: &ready) }
        var ordered: [SyncEvent] = []; ordered.reserveCapacity(events.count)
        while !ready.isEmpty {
            let event = removeFirst(from: &ready); ordered.append(event)
            for child in successors[event.identity] ?? [] {
                dependencies[child]?.remove(event.identity)
                if dependencies[child]?.isEmpty == true, let next = events[child] { insert(next, into: &ready) }
            }
        }
        guard ordered.count == events.count else { throw HistoryError.causalCycle }
        return ordered
    }
    private static func insert(_ event: SyncEvent, into heap: inout [SyncEvent]) {
        heap.append(event)
        var index = heap.count - 1
        while index > 0 {
            let parent = (index - 1) / 2
            guard precedes(heap[index], heap[parent]) else { break }
            heap.swapAt(index, parent); index = parent
        }
    }
    private static func removeFirst(from heap: inout [SyncEvent]) -> SyncEvent {
        heap.swapAt(0, heap.count - 1)
        let first = heap.removeLast()
        var index = 0
        while index * 2 + 1 < heap.count {
            var child = index * 2 + 1
            if child + 1 < heap.count, precedes(heap[child + 1], heap[child]) { child += 1 }
            guard precedes(heap[child], heap[index]) else { break }
            heap.swapAt(index, child); index = child
        }
        return first
    }
    private static func precedes(_ lhs: SyncEvent, _ rhs: SyncEvent) -> Bool {
        if lhs.studyDay != rhs.studyDay { return lhs.studyDay < rhs.studyDay }
        let leftTime = lhs.clockQuality == .trusted ? lhs.timestamp : 0
        let rightTime = rhs.clockQuality == .trusted ? rhs.timestamp : 0
        if leftTime != rightTime { return leftTime < rightTime }
        return lhs.identity < rhs.identity
    }
}

public struct JournalMutation: Equatable, Sendable {
    public static let maximumBodySize = 65536
    public let event: SyncEvent
    public let body: Data
    public init(event: SyncEvent, body: Data) throws {
        guard body.count <= Self.maximumBodySize, Data(SHA256.hash(data: body)) == event.bodyHash else { throw VaultError.integrity }
        self.event = event; self.body = body
    }
}

extension EventIdentity {
    var storageKey: Data {
        var key = origin; key.reserveCapacity(32)
        key.appendLittleEndian(epoch, count: 8); key.appendLittleEndian(sequence, count: 8)
        return key
    }
}

import Foundation

public enum LegacyMarkLogError: Error, Equatable, Sendable {
    case invalidHeader, tooLarge, truncated, invalidRecord(Int), capacityExceeded(Int), unknownKey(UInt32), ambiguousKey(UInt32), invalidResolution(UInt32)
}
public enum TintaReadingSnapshotError: Error, Equatable, Sendable {
    case invalidMapping, unknownIdentity(UInt32), partialCollision(UInt32)
}
public struct LegacyReadingConflict: Equatable, Sendable {
    public let legacyKey: UInt32
    public let candidates: [UInt32]
}
public struct LegacyReadingConflictDetails: Equatable, Identifiable, Sendable {
    public var id: UInt32 { legacyKey }
    public let legacyKey: UInt32
    public let candidates: [CourseStoryContext]
}
public struct LegacyMarkLog: Sendable {
    public let originalBytes: Data
    public let keys: [UInt32]
    public let recordCount: Int
    public init(bytes: Data) throws {
        let bytes = Data(bytes)
        guard bytes.count <= LegacyTintaJournal.maximumByteCount else { throw LegacyMarkLogError.tooLarge }
        guard bytes.count >= 4, bytes.prefix(4) == Data("TMK1".utf8) else { throw LegacyMarkLogError.invalidHeader }
        guard (bytes.count - 4) % 8 == 0 else { throw LegacyMarkLogError.truncated }
        var reader = ByteReader(bytes); _ = try reader.take(4)
        var keys: [UInt32] = []; keys.reserveCapacity(96)
        let count = (bytes.count - 4) / 8
        for index in 0..<count {
            let packed = try reader.take(8)
            var record = ByteReader(packed)
            let key = UInt32(try record.number(4)), operation = try record.number(1)
            guard try record.number(1) == 0, (1...2).contains(operation),
                  try record.number(2) == UInt64(legacyCRC32(Data(packed.prefix(6))) & 0xffff) else {
                throw LegacyMarkLogError.invalidRecord(index)
            }
            if operation == 1, !keys.contains(key) {
                guard keys.count < 96 else { throw LegacyMarkLogError.capacityExceeded(index) }
                keys.append(key)
            } else if operation == 2, let position = keys.firstIndex(of: key) {
                keys.remove(at: position)
            }
        }
        originalBytes = bytes; self.keys = keys; recordCount = count
    }
}

public extension LegacyMarkLog {
    static func encodeCompletedReadings(identities: Set<UInt32>, course: CoursePackMetadata) throws -> Data {
        guard course.storyIdentities.count == course.legacyStoryIdentities.count,
              Set(course.storyIdentities).count == course.storyIdentities.count else {
            throw TintaReadingSnapshotError.invalidMapping
        }
        let known = Set(course.storyIdentities)
        if let unknown = identities.subtracting(known).min() {
            throw TintaReadingSnapshotError.unknownIdentity(unknown)
        }
        var mapping: [UInt32: Set<UInt32>] = [:]
        mapping.reserveCapacity(course.storyIdentities.count)
        for (identity, key) in zip(course.storyIdentities, course.legacyStoryIdentities) {
            mapping[key, default: []].insert(identity)
        }
        var keys = Set<UInt32>()
        keys.reserveCapacity(min(96, identities.count))
        for key in mapping.keys.sorted() {
            let group = mapping[key]!
            let selected = group.intersection(identities)
            guard selected.isEmpty || selected == group else {
                throw TintaReadingSnapshotError.partialCollision(key)
            }
            if !selected.isEmpty { keys.insert(key) }
        }
        return try encodeSnapshot(keys: keys)
    }

    static func encodeSnapshot(keys: Set<UInt32>) throws -> Data {
        guard keys.count <= 96 else { throw LegacyMarkLogError.capacityExceeded(keys.count) }
        var output = Data("TMK1".utf8)
        output.reserveCapacity(4 + keys.count * 8)
        for key in keys.sorted() {
            var record = Data()
            record.reserveCapacity(8)
            record.appendLittleEndian(UInt64(key), count: 4)
            record.append(contentsOf: [1, 0])
            record.appendLittleEndian(UInt64(legacyCRC32(record) & 0xffff), count: 2)
            output.append(record)
        }
        return output
    }

    private func storyMapping(course: CoursePackMetadata) throws -> [UInt32: [UInt32]] {
        guard course.legacyStoryIdentities.count == course.storyIdentities.count else { throw StoreError.invalidValue }
        var mapping: [UInt32: [UInt32]] = [:]; mapping.reserveCapacity(course.storyIdentities.count)
        for (legacy, identity) in zip(course.legacyStoryIdentities, course.storyIdentities) {
            mapping[legacy, default: []].append(identity)
        }
        return mapping
    }
    func readingConflicts(course: CoursePackMetadata) throws -> [LegacyReadingConflict] {
        let mapping = try storyMapping(course: course)
        var conflicts: [LegacyReadingConflict] = []; conflicts.reserveCapacity(keys.count)
        for key in keys {
            guard let matches = mapping[key] else { throw LegacyMarkLogError.unknownKey(key) }
            if matches.count > 1 { conflicts.append(LegacyReadingConflict(legacyKey: key, candidates: matches)) }
        }
        return conflicts
    }
    func completedStoryIdentities(course: CoursePackMetadata,
                                  confirmedResolutions: [UInt32: Set<UInt32>] = [:]) throws -> [UInt32] {
        let mapping = try storyMapping(course: course)
        for (key, selected) in confirmedResolutions {
            guard keys.contains(key), let matches = mapping[key], matches.count > 1,
                  selected.isSubset(of: Set(matches)) else { throw LegacyMarkLogError.invalidResolution(key) }
        }
        var identities: [UInt32] = []; identities.reserveCapacity(course.storyIdentities.count)
        for key in keys {
            guard let matches = mapping[key] else { throw LegacyMarkLogError.unknownKey(key) }
            if matches.count == 1 { identities.append(matches[0]) }
            else {
                guard let selected = confirmedResolutions[key] else { throw LegacyMarkLogError.ambiguousKey(key) }
                identities.append(contentsOf: selected.sorted())
            }
        }
        return identities
    }
}

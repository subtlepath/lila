import Foundation
import CTintaScheduler

public struct SchedulerConfiguration: Equatable, Sendable {
    public let retentionBasisPoints: UInt16
    public let maximumInterval: UInt16
    public init(retentionBasisPoints: UInt16 = 9000, maximumInterval: UInt16 = 365) throws {
        guard (1 ... 9999).contains(retentionBasisPoints), maximumInterval > 0 else { throw ProtocolError.value }
        self.retentionBasisPoints = retentionBasisPoints; self.maximumInterval = maximumInterval
    }
    // Version one fixes the firmware's FSRS-6 default weights and learning rules.
    public var encoded: Data {
        var bytes = Data([1, 1])
        bytes.appendLittleEndian(UInt64(retentionBasisPoints), count: 2)
        bytes.appendLittleEndian(UInt64(maximumInterval), count: 2)
        return bytes
    }
}

public struct ScheduledItem: Equatable, Sendable {
    public enum Flag: Sendable { case suspension, star }
    public let bytes: Data
    public init(uid: UInt32) throws {
        var output = Data(count: 16)
        guard output.withUnsafeMutableBytes({ tinta_scheduler_fresh(uid, $0.bindMemory(to: UInt8.self).baseAddress) }) else { throw ProtocolError.value }
        bytes = output
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == 16, bytes.withUnsafeBytes({ tinta_scheduler_validate($0.bindMemory(to: UInt8.self).baseAddress) }) else { throw ProtocolError.value }
        self.bytes = Data(bytes)
    }
    public func settingFlag(_ flag: Flag, enabled: Bool) throws -> Self {
        var output = Data(count: 16)
        let valid = bytes.withUnsafeBytes { input in
            output.withUnsafeMutableBytes { destination in
                tinta_scheduler_set_flag(input.bindMemory(to: UInt8.self).baseAddress, flag == .star, enabled,
                                         destination.bindMemory(to: UInt8.self).baseAddress)
            }
        }
        guard valid else { throw ProtocolError.value }
        return try Self(decoding: output)
    }
    public func reviewed(grade: UInt8, day: UInt16, configuration: SchedulerConfiguration) throws -> Self {
        try reviewedWithCounts(grade: grade, day: day, configuration: configuration).item
    }
    public func reviewedWithCounts(grade: UInt8, day: UInt16, configuration: SchedulerConfiguration)
        throws -> (item: Self, newItem: Bool, review: Bool, correct: Bool) {
        var output = Data(count: 16)
        var counts = TintaReviewCounts()
        let valid = bytes.withUnsafeBytes { input in
            output.withUnsafeMutableBytes { destination in
                tinta_scheduler_review_counted(input.bindMemory(to: UInt8.self).baseAddress, grade, day,
                                       configuration.retentionBasisPoints, configuration.maximumInterval,
                                       destination.bindMemory(to: UInt8.self).baseAddress, &counts)
            }
        }
        guard valid else { throw ProtocolError.value }
        return (try Self(decoding: output), counts.new_item, counts.review, counts.correct)
    }
}

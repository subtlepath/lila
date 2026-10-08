import Foundation

public enum LegacyProfileError: Error, Equatable, Sendable { case corrupt, unsupported, invalidField(Int), invalidLessonProgress }
public struct LegacyProfile: Sendable {
    public let originalBytes: Data
    public let scheduler: SchedulerConfiguration
    public let newPerDay: UInt16
    public let reviewCap: UInt16
    public let sessionSize: UInt16
    public let textSize: UInt8
    public let language: UInt8
    public let showVulgar: Bool
    public let typedAnswers: Bool
    public let rolloverHour: UInt8
    public let utcOffsetMinutes: Int16
    public let lastConfirmedDay: UInt16
    public let currentLesson: UInt16
    public let unlockedThrough: UInt16
    public init(bytes: Data) throws {
        let bytes = Data(bytes)
        guard bytes.count >= 12 else { throw LegacyProfileError.corrupt }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data("TPRF".utf8) else { throw LegacyProfileError.corrupt }
        let version = try reader.number(2), length = Int(try reader.number(2))
        guard bytes.count == length + 12 else { throw LegacyProfileError.corrupt }
        var checksum = ByteReader(Data(bytes[(8 + length)..<bytes.count]))
        guard try checksum.number(4) == UInt64(legacyCRC32(Data(bytes[0..<(8 + length)]))) else { throw LegacyProfileError.corrupt }
        guard version == 1, length == 31 else { throw LegacyProfileError.unsupported }
        let payload = try reader.take(length)
        func field(_ offset: Int, _ count: Int, _ range: ClosedRange<UInt64>) throws -> UInt64 {
            var field = ByteReader(Data(payload[offset..<offset + count]))
            let value = try field.number(count)
            guard range.contains(value) else { throw LegacyProfileError.invalidField(offset) }
            return value
        }
        newPerDay = UInt16(try field(0, 2, 0...200)); reviewCap = UInt16(try field(2, 2, 0...9999))
        let retention = UInt16(try field(4, 2, 700...970)), interval = UInt16(try field(6, 2, 1...36500))
        scheduler = try SchedulerConfiguration(retentionBasisPoints: retention * 10, maximumInterval: interval)
        sessionSize = UInt16(try field(8, 2, 5...500)); textSize = UInt8(try field(10, 1, 0...2))
        language = UInt8(try field(11, 1, 0...2)); showVulgar = try field(12, 1, 0...1) != 0
        _ = try field(13, 1, 1...50); _ = try field(14, 2, 30...3600); _ = try field(16, 1, 0...2)
        rolloverHour = UInt8(try field(17, 1, 0...23))
        utcOffsetMinutes = Int16(bitPattern: UInt16(try field(18, 2, 0...65535)))
        guard (-720...840).contains(utcOffsetMinutes) else { throw LegacyProfileError.invalidField(18) }
        lastConfirmedDay = UInt16(try field(20, 2, 0...65535)); currentLesson = UInt16(try field(22, 2, 0...65535))
        unlockedThrough = UInt16(try field(24, 2, 0...65535))
        _ = try field(26, 1, 0...100); _ = try field(27, 1, 0...100)
        typedAnswers = try field(28, 1, 0...1) != 0; _ = try field(29, 2, 0...65535)
        originalBytes = bytes
    }
}

public extension LegacyProfile {
    // Present for confirmation before creating shared preference events.
    func portablePreferences() throws -> [PreferenceBody] {
        [try PreferenceBody(key: .tintaNewPerDay, value: .integer(Int32(newPerDay))),
         try PreferenceBody(key: .tintaReviewCap, value: .integer(Int32(reviewCap))),
         try PreferenceBody(key: .tintaRetentionPermille, value: .integer(Int32(scheduler.retentionBasisPoints / 10))),
         try PreferenceBody(key: .tintaMaximumInterval, value: .integer(Int32(scheduler.maximumInterval))),
         try PreferenceBody(key: .tintaSessionSize, value: .integer(Int32(sessionSize))),
         try PreferenceBody(key: .tintaTextSize, value: .integer(Int32(textSize))),
         try PreferenceBody(key: .tintaLanguage, value: .integer(Int32(language))),
         try PreferenceBody(key: .tintaShowVulgar, value: .integer(showVulgar ? 1 : 0)),
         try PreferenceBody(key: .tintaTypedAnswers, value: .integer(typedAnswers ? 1 : 0))]
    }
}

public extension LegacyProfile {
    // Unlocking lessons independently does not mark them completed.
    func completedLessonIndices(lessonCount: UInt16) throws -> Range<UInt16> {
        guard currentLesson <= lessonCount,
              lessonCount == 0 ? unlockedThrough == 0 : unlockedThrough < lessonCount else {
            throw LegacyProfileError.invalidLessonProgress
        }
        return 0..<currentLesson
    }
}

public extension LegacyProfile {
    func completedLessonIdentities(course: CoursePackMetadata) throws -> [UInt32] {
        guard course.lessonIdentities.count == Int(course.lessonCount) else { throw LegacyProfileError.invalidLessonProgress }
        let indices = try completedLessonIndices(lessonCount: course.lessonCount)
        return Array(course.lessonIdentities.prefix(Int(indices.upperBound)))
    }
}

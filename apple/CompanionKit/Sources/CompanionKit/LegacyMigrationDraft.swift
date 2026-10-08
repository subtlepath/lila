import Foundation

public struct LegacyReadingResolutionDraft: Codable, Equatable, Sendable {
    public let legacyKey: UInt32
    public let identities: [UInt32]
}

public struct LegacyMigrationDraft: Codable, Equatable, Sendable {
    public let version: Int
    public let backup: ContentID
    public let course: ContentID
    public let confirmedCourseIdentity: Data
    public let retentionBasisPoints: UInt16
    public let maximumInterval: UInt16
    public let readingResolutions: [LegacyReadingResolutionDraft]
    public let independentBackups: [ContentID]
    public let reviewedBackups: [ContentID]

    init(backup: ContentID, course: ContentID, confirmedCourseIdentity: Data, configuration: SchedulerConfiguration,
         readings: [UInt32: Set<UInt32>], independent: Set<ContentID>, reviewed: Set<ContentID>) {
        version = 1; self.backup = backup; self.course = course; self.confirmedCourseIdentity = confirmedCourseIdentity
        retentionBasisPoints = configuration.retentionBasisPoints; maximumInterval = configuration.maximumInterval
        readingResolutions = readings.keys.sorted().map { LegacyReadingResolutionDraft(legacyKey: $0, identities: readings[$0]!.sorted()) }
        independentBackups = independent.sorted { $0.hex < $1.hex }
        reviewedBackups = reviewed.sorted { $0.hex < $1.hex }
    }
    public var confirmedReadingResolutions: [UInt32: Set<UInt32>] {
        var values: [UInt32: Set<UInt32>] = [:]; values.reserveCapacity(readingResolutions.count)
        for reading in readingResolutions { values[reading.legacyKey] = Set(reading.identities) }
        return values
    }
    func validate() throws {
        _ = try SchedulerConfiguration(retentionBasisPoints: retentionBasisPoints, maximumInterval: maximumInterval)
        guard version == 1, confirmedCourseIdentity.count == 16, confirmedCourseIdentity.contains(where: { $0 != 0 }),
              Set(readingResolutions.map(\.legacyKey)).count == readingResolutions.count,
              Set(independentBackups).count == independentBackups.count,
              Set(reviewedBackups).count == reviewedBackups.count, reviewedBackups.contains(backup),
              !independentBackups.contains(backup), Set(independentBackups).isSubset(of: Set(reviewedBackups)) else {
            throw StoreError.invalidValue
        }
        for resolution in readingResolutions {
            guard Set(resolution.identities).count == resolution.identities.count,
                  resolution.identities.allSatisfy({ $0 > 0 && $0 < UInt32.max }) else { throw StoreError.invalidValue }
        }
    }
}

import Foundation

public struct TintaDerivedCourseFiles: Equatable, Sendable {
    public let course: Data
    public let studyDay: UInt16
    public let itemState: Data
    public let localReviews: Data
    public let completedLessons: Data
    public let completedReadings: Data
    public let dayLog: Data
}

public extension TintaSnapshot {
    func derivedCourseFiles(course: Data, studyDay: UInt16) throws -> TintaDerivedCourseFiles {
        guard course.count == 16, course.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        var lessons = Set<UInt32>(), readings = Set<UInt32>()
        lessons.reserveCapacity(completedLessons.count)
        readings.reserveCapacity(completedReadings.count)
        for subject in completedLessons where subject.course == course { lessons.insert(subject.uid) }
        for subject in completedReadings where subject.course == course { readings.insert(subject.uid) }
        var courseItems: [UInt32: ScheduledItem] = [:]
        courseItems.reserveCapacity(min(items.count, 0x7fff))
        for (subject, item) in items where subject.course == course { courseItems[subject.uid] = item }
        let daily = studyTotals[course]?[studyDay] ?? StudyTotals()
        return try TintaDerivedCourseFiles(
            course: Data(course),
            studyDay: studyDay,
            itemState: LegacyItemStore.encodeSnapshot(items: courseItems, studyDay: studyDay,
                                                     newItems: UInt16(clamping: daily.newItems),
                                                     reviews: UInt16(clamping: daily.reviews)),
            localReviews: Data(),
            completedLessons: TintaCompletionSet(kind: .lessons, identities: lessons).encoded,
            completedReadings: TintaCompletionSet(kind: .readings, identities: readings).encoded,
            dayLog: TintaDayLog.encode(studyTotals[course] ?? [:]))
    }
}

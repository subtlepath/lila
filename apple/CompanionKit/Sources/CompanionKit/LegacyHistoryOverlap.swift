import Foundation

public enum LegacySharedHistoryError: Error, Equatable, Sendable {
    case notIdentical, alreadyPrepared, canonicalNotInstalled, invalidReceipt, alreadyShared(ContentID)
}

public enum LegacyOverlapEvidence: Equatable, Sendable {
    case identicalLearnerFiles
    case sharedReviewPrefix(records: Int)
}

public struct LegacyHistoryOverlap: Equatable, Sendable {
    public let firstBackup: ContentID
    public let secondBackup: ContentID
    public let firstReader: Data
    public let secondReader: Data
    public let course: Data
    public let evidence: LegacyOverlapEvidence
}

public extension ContentVault {
    // Evidence of overlap requires confirmation; it does not establish shared event provenance.
    func legacyHistoryOverlap(first: ContentID, second: ContentID) throws -> LegacyHistoryOverlap? {
        guard first != second else { throw StoreError.invalidValue }
        let firstManifest = try verifiedLegacyBackup(first), secondManifest = try verifiedLegacyBackup(second)
        guard firstManifest.course == secondManifest.course else { return nil }
        let left = try legacyBackupSnapshot(first), right = try legacyBackupSnapshot(second)
        let firstFiles = firstManifest.files.filter { $0.role != .usage }
        let secondFiles = secondManifest.files.filter { $0.role != .usage }
        let evidence: LegacyOverlapEvidence
        if firstFiles == secondFiles {
            evidence = .identicalLearnerFiles
        } else {
            var count = 0
            for (a, b) in zip(left.journal.entries, right.journal.entries) {
                guard a.bytes == b.bytes else { break }
                count += 1
            }
            guard count > 0 else { return nil }
            evidence = .sharedReviewPrefix(records: count)
        }
        return LegacyHistoryOverlap(firstBackup: first, secondBackup: second,
            firstReader: firstManifest.reader, secondReader: secondManifest.reader,
            course: firstManifest.course, evidence: evidence)
    }
}

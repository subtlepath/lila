import Foundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif

public struct TintaDerivedInstallation: Equatable, Sendable {
    public let files: TintaDerivedCourseFiles
    public let manifest: Data
    public let frontierHash: Data

    /// Checks artifact integrity; does not prove journal authority or pack membership.
    public init(files: TintaDerivedCourseFiles, manifest: Data) throws {
        let receipt = try TintaDerivedReceipt(decoding: manifest)
        guard receipt.course == files.course, receipt.studyDay == files.studyDay else { throw VaultError.integrity }
        for (index, bytes) in [files.itemState, files.localReviews, files.completedLessons,
                               files.completedReadings, files.dayLog].enumerated() {
            guard receipt.files[index].length == UInt64(bytes.count),
                  receipt.files[index].hash == Data(SHA256.hash(data: bytes)) else { throw VaultError.integrity }
        }
        self.files = files
        self.manifest = manifest
        self.frontierHash = receipt.frontierHash
    }
}

public extension TintaHistory {
    /// Input is the complete authoritative journal, including other courses.
    static func derivedInstallation(_ deliveries: [JournalMutation], course: Data, metadata: CoursePackMetadata, studyDay: UInt16,
                                    storageGeneration: Data, snapshotIdentity: Data,
                                    packHash: Data, revision: UInt64) throws -> TintaDerivedInstallation {
        let activeItems = Set(metadata.itemIdentities)
        let lessons = Set(metadata.lessonIdentities)
        let readings = Set(metadata.storyIdentities)
        for mutation in deliveries where mutation.event.kind.rawValue >= SyncEventKind.review.rawValue {
            let body = try TintaBody(mutation: mutation)
            guard body.subject.course == course else { continue }
            let uid = body.subject.uid
            let known: Bool
            switch body.value {
            case .lessonComplete: known = lessons.contains(uid)
            case .readingComplete: known = readings.contains(uid)
            case .review, .undo, .suspension, .star:
                known = metadata.itemHistoryCount.map { uid > 0 && uid != UInt32.max && uid <= $0 }
                    ?? activeItems.contains(uid)
            }
            guard known else { throw ProtocolError.value }
        }
        let frontier = try TintaJournalFrontier.digest(deliveries)
        let files = try replay(deliveries, course: course).derivedCourseFiles(course: course, studyDay: studyDay)
        let manifest = try files.installationManifest(storageGeneration: storageGeneration,
            snapshotIdentity: snapshotIdentity, packHash: packHash, frontierHash: frontier, revision: revision)
        return try TintaDerivedInstallation(files: files, manifest: manifest)
    }
}

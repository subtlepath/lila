import Foundation

public enum CourseTransferAdmission: Equatable, Sendable {
    case compatible, identityBaseline, readerValidationRequired, explicitSwitch
}
public enum CourseTransferAdmissionError: Error, Equatable, Sendable {
    case multipleActiveCourses, differentCourse
    case incompatibleHistory(CoursePackInspector.ItemHistoryCompatibility)
}

public extension LibraryStore {
    func admitCourseTransfer(_ candidate: ContentID, inventory: ReaderInventory,
                             vault: ContentVault, confirmedSwitchJob: UUID? = nil) async throws -> CourseTransferAdmission {
        guard inventory.complete else { throw ContentReconciliationError.incompleteInventory }
        let proposed = try courseManifest(candidate)
        let object = try await vault.verifiedObject(candidate)
        guard object.length == proposed.length else { throw VaultError.integrity }
        let metadata = try CoursePackInspector.inspect(object.url)
        guard UInt32(metadata.major) == proposed.formatVersion,
              try coursePackDetails(candidate) == CoursePackDetails(metadata) else { throw VaultError.integrity }
        let installed = inventory.contents.filter { $0.kind == .course }
        guard installed.count <= 1 else { throw CourseTransferAdmissionError.multipleActiveCourses }
        guard let current = installed.first else { return .readerValidationRequired }
        if current.logicalIdentity.contains(where: { $0 != 0 }), current.logicalIdentity != proposed.logicalIdentity {
            if let id = confirmedSwitchJob, let job = try job(id),
               let consent = try courseSwitchConfirmation(id), job.content == candidate,
               job.reader == inventory.reader, job.storageGeneration == inventory.generation,
               consent.previousCourse == current.logicalIdentity, consent.previousHash == current.content.digest,
               consent.nextCourse == proposed.logicalIdentity, consent.nextHash == proposed.content.digest {
                return .explicitSwitch
            }
            throw CourseTransferAdmissionError.differentCourse
        }
        guard current.content != candidate else {
            guard current.length == proposed.length, current.formatVersion == proposed.formatVersion else {
                throw VaultError.integrity
            }
            return .compatible
        }
        guard let known = try content(current.content), known.kind == .course else { return .readerValidationRequired }
        let previous = try await vault.verifiedObject(current.content)
        let previousMetadata = try CoursePackInspector.inspect(previous.url)
        guard previous.length == current.length, known.length == current.length,
              UInt32(previousMetadata.major) == current.formatVersion else { throw VaultError.integrity }
        if let identity = try courseIdentity(current.content), current.logicalIdentity.contains(where: { $0 != 0 }),
           identity != current.logicalIdentity {
            throw VaultError.integrity
        }
        let result = try CoursePackInspector.compareItemHistory(current: previous.url, candidate: object.url)
        switch result {
        case .compatible: return .compatible
        case .identityBaseline: return .identityBaseline
        case .missingHistory: return .readerValidationRequired
        default: throw CourseTransferAdmissionError.incompatibleHistory(result)
        }
    }
}

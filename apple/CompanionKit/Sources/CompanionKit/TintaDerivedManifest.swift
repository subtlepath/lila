import Foundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif

public extension TintaDerivedCourseFiles {
    func installationManifest(storageGeneration: Data, snapshotIdentity: Data, packHash: Data,
                              frontierHash: Data, revision: UInt64) throws -> Data {
        for identity in [course, storageGeneration, snapshotIdentity] {
            guard identity.count == 16, identity.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        }
        for digest in [packHash, frontierHash] {
            guard digest.count == 32, digest.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        }
        guard revision > 0 else { throw ProtocolError.value }
        var bytes = Data("TDS1".utf8)
        bytes.reserveCapacity(332)
        bytes.append(course); bytes.append(packHash); bytes.append(frontierHash)
        bytes.append(snapshotIdentity); bytes.append(storageGeneration)
        bytes.appendLittleEndian(UInt64(studyDay), count: 2)
        bytes.appendLittleEndian(0, count: 2)
        bytes.appendLittleEndian(revision, count: 8)
        for file in [itemState, localReviews, completedLessons, completedReadings, dayLog] {
            bytes.appendLittleEndian(UInt64(file.count), count: 8)
            bytes.append(Data(SHA256.hash(data: file)))
        }
        bytes.appendLittleEndian(UInt64(legacyCRC32(bytes)), count: 4)
        return bytes
    }
}

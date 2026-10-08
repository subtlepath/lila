import Foundation

public enum FirmwareTransferAdmissionError: Error, Equatable, Sendable {
    case missingMetadata, incompatibleReader, changedMetadata
}

public extension LibraryStore {
    func admitFirmwareTransfer(_ candidate: ContentID, device: DeviceDescriptor,
                               info: FirmwareReaderInfo, vault: ContentVault) async throws -> StoredObject {
        let fresh = try info.refreshedDevice(device)
        guard let metadata = try firmwareCompatibility(candidate) else {
            throw FirmwareTransferAdmissionError.missingMetadata
        }
        guard metadata.chipID == info.chip else { throw FirmwareTransferAdmissionError.incompatibleReader }
        return try await admitFirmwareTransfer(candidate, device: fresh, readerStateSchema: info.stateSchema,
            readerPartitionBytes: info.partitionBytes, readerHeaderVersions: info.headerVersions, vault: vault)
    }
    /// Caller supplies fresh authenticated reader state; admission alone does not authorize flashing.
    func admitFirmwareTransfer(_ candidate: ContentID, device: DeviceDescriptor,
                               readerStateSchema: UInt32?, readerPartitionBytes: UInt64?,
                               readerHeaderVersions: [UInt8]?, vault: ContentVault) async throws -> StoredObject {
        guard let metadata = try firmwareCompatibility(candidate) else {
            throw FirmwareTransferAdmissionError.missingMetadata
        }
        guard metadata.permits(device: device, readerStateSchema: readerStateSchema,
                               readerPartitionBytes: readerPartitionBytes, readerHeaderVersions: readerHeaderVersions) else {
            throw FirmwareTransferAdmissionError.incompatibleReader
        }
        let object = try await vault.verifiedObject(candidate)
        try Task.checkCancellation()
        guard object.length == metadata.length else { throw VaultError.integrity }
        _ = try FirmwareImageInspector.inspect(object.url, compatibility: metadata)
        guard try firmwareCompatibility(candidate) == metadata else {
            throw FirmwareTransferAdmissionError.changedMetadata
        }
        return object
    }
}

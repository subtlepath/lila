import Foundation

/// Durable transfer receipt for verified SD staging. It does not prove flashing or boot success.
public struct FirmwareStagingReceipt: Equatable, Sendable {
    public let transaction: UUID
    public let reader: Data
    public let generation: Data
    public let installation: Data
    public let image: ContentID
    public let length: UInt64
    init(job: TransferJob, length: UInt64) {
        transaction = job.id; reader = job.reader; generation = job.storageGeneration
        installation = job.installation; image = job.content; self.length = length
    }
}

/// Compatibility snapshot captured before BLE yields to Wi-Fi. Flashing requires a fresh check.
public struct FirmwareHandoffPreparation: Sendable {
    public let job: TransferJob
    let info: FirmwareReaderInfo
    init(job: TransferJob, info: FirmwareReaderInfo) {
        self.job = job; self.info = info
    }
}

/// Durable installation authorization, independent of the completed SD transfer.
public struct FirmwareInstallation: Equatable, Sendable {
    public let receipt: FirmwareStagingReceipt
    public let request: FirmwareInstallRequest
    public let bootVerified: Bool
}

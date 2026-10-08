import Foundation

/// Exact staged image and release constraints. Encoding alone never authorizes flashing.
public struct FirmwareInstallRequest: Equatable, Sendable {
    public let bytes: Data
    public init(receipt: FirmwareStagingReceipt, metadata: FirmwareReleaseCompatibility,
                device: DeviceDescriptor, info: FirmwareReaderInfo) throws {
        let fresh = try info.refreshedDevice(device)
        guard receipt.reader == device.identity, receipt.generation == device.storageGeneration,
              receipt.image.digest == metadata.sha256, receipt.length == metadata.length,
              receipt.length >= 65536, metadata.chipID == info.chip,
              metadata.permits(device: fresh, readerStateSchema: info.stateSchema,
                readerPartitionBytes: info.partitionBytes, readerHeaderVersions: info.headerVersions),
              let range = metadata.companionProtocol else { throw FirmwareTransferAdmissionError.incompatibleReader }
        var output = Data([0x46, 0x57, 0x46, 1])
        output.reserveCapacity(104)
        output.append(receipt.generation)
        output.append(withUnsafeBytes(of: receipt.transaction.uuid) { Data($0) })
        output.append(metadata.sha256); output.appendLittleEndian(receipt.length, count: 8)
        output.appendLittleEndian(UInt64(metadata.stateSchema.minimum), count: 4)
        output.appendLittleEndian(UInt64(metadata.stateSchema.maximum), count: 4)
        let mask = metadata.journals.supportedJournalHeaderVersions.reduce(UInt32(0)) { $0 | (1 << ($1 - 1)) }
        output.appendLittleEndian(UInt64(mask), count: 4)
        output.append(UInt8(range.minimum)); output.append(UInt8(range.maximum))
        output.append(metadata.minimumBatteryPercent); output.append(device.board.rawValue)
        output.appendLittleEndian(UInt64(metadata.chipID), count: 2); output.append(Data(count: 2))
        output.appendLittleEndian(metadata.otaPartitionBytes, count: 8)
        try self.init(decoding: output)
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == 104 else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x46, 0x57, 0x46, 1]) else { throw ProtocolError.version }
        for count in [16, 16, 32] {
            guard try reader.take(count).contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        }
        let length = try reader.number(8), minimumSchema = try reader.number(4), maximumSchema = try reader.number(4)
        let journals = try reader.number(4), minimumProtocol = try reader.number(1), maximumProtocol = try reader.number(1)
        let battery = try reader.number(1), board = try reader.number(1), chip = try reader.number(2)
        guard try reader.number(2) == 0 else { throw ProtocolError.value }
        let partition = try reader.number(8)
        guard length >= 65536, length <= partition, partition <= UInt32.max, minimumSchema <= maximumSchema,
              journals & ~UInt64(7) == 0, minimumProtocol == 1, maximumProtocol >= 1,
              (30 ... 100).contains(battery), (1 ... 5).contains(board), chip == (board == 1 ? 5 : 9) else {
            throw ProtocolError.value
        }
        self.bytes = bytes
    }
}

public enum FirmwareInstallCommandError: Error, Equatable, Sendable {
    case control(UInt8), rejected(UInt8)
}
public extension FirmwareInstallRequest {
    func controlFrame(requestID: UInt32) throws -> ControlFrame {
        try ControlFrame(command: .installFirmware, requestID: requestID, payload: bytes)
    }
    func validateAcceptance(_ reply: ControlFrame, request: ControlFrame) throws {
        guard request.command == .installFirmware, !request.response, request.payload == bytes,
              reply.response, reply.requestID == request.requestID else { throw ProtocolError.value }
        if reply.command == .error {
            guard reply.payload.count == 1 else { throw ProtocolError.length }
            throw FirmwareInstallCommandError.control(reply.payload[0])
        }
        guard reply.command == .installFirmware, reply.payload.count == 17,
              reply.payload.dropFirst() == bytes.subdata(in: 20..<36) else { throw ProtocolError.value }
        let result = reply.payload[0]
        guard result <= 5 else { throw ProtocolError.value }
        guard result == 0 else { throw FirmwareInstallCommandError.rejected(result) }
    }
}
public extension AuthenticatedReaderSession {
    /// Acceptance is durable authorization; reconnect and verify the running hash to prove success.
    func installFirmware(_ installation: FirmwareInstallRequest, requestID: UInt32) async throws {
        try Task.checkCancellation()
        guard installation.bytes.subdata(in: 4..<20) == device.storageGeneration,
              installation.bytes[91] == device.board.rawValue else { throw ProtocolError.value }
        let request = try installation.controlFrame(requestID: requestID)
        try installation.validateAcceptance(await exchange(request), request: request)
    }
}

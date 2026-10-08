import Foundation

public struct FirmwareReaderInfo: Equatable, Sendable {
    public let board: Board
    public let battery: UInt8
    public let minimumProtocol: UInt8
    public let maximumProtocol: UInt8
    public let generation: Data
    public let runningBuild: Data
    public let chip: UInt16
    public let partitionBytes: UInt64
    public let stateSchema: UInt32
    public let journalVersions: UInt32
    public var headerVersions: [UInt8] {
        (1 ... 3).compactMap { journalVersions & (1 << ($0 - 1)) == 0 ? nil : UInt8($0) }
    }
    public init(decoding bytes: Data) throws {
        guard bytes.count == 76 else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Data([0x46, 0x57, 0x49, 1]) else { throw ProtocolError.version }
        guard let board = Board(rawValue: UInt8(try reader.number(1))) else { throw ProtocolError.value }
        self.board = board
        battery = UInt8(try reader.number(1)); minimumProtocol = UInt8(try reader.number(1))
        maximumProtocol = UInt8(try reader.number(1))
        generation = try reader.take(16); runningBuild = try reader.take(32)
        chip = UInt16(try reader.number(2))
        guard try reader.number(2) == 0 else { throw ProtocolError.value }
        partitionBytes = try reader.number(8); stateSchema = UInt32(try reader.number(4))
        journalVersions = UInt32(try reader.number(4))
        guard battery <= 100, minimumProtocol > 0, minimumProtocol <= maximumProtocol,
              generation.contains(where: { $0 != 0 }), runningBuild.contains(where: { $0 != 0 }),
              chip != 0xffff, partitionBytes >= 65536, partitionBytes <= UInt32.max,
              journalVersions & ~UInt32(7) == 0 else { throw ProtocolError.value }
    }
    public func refreshedDevice(_ device: DeviceDescriptor) throws -> DeviceDescriptor {
        guard generation == device.storageGeneration, board == device.board, runningBuild == device.runningBuild,
              minimumProtocol == device.minimumProtocol, maximumProtocol == device.maximumProtocol else {
            throw ReaderSessionError.wrongReader
        }
        var bytes = device.encoded
        bytes[39] = battery
        return try DeviceDescriptor(decoding: bytes)
    }
    public static func request(generation: Data, requestID: UInt32) throws -> ControlFrame {
        guard generation.count == 16, generation.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        return try ControlFrame(command: .installFirmware, requestID: requestID,
            payload: Data([0x46, 0x57, 0x51, 1]) + generation)
    }
    public static func decode(_ reply: ControlFrame, request: ControlFrame) throws -> Self {
        guard !request.response, request.command == .installFirmware, request.payload.count == 20,
              request.payload.prefix(4) == Data([0x46, 0x57, 0x51, 1]), reply.response,
              reply.requestID == request.requestID else { throw ReaderSessionError.invalidResponse }
        if reply.command == .error, reply.payload.count == 1 { throw ReaderSessionError.control(reply.payload[0]) }
        guard reply.command == .installFirmware else { throw ReaderSessionError.invalidResponse }
        let info = try Self(decoding: reply.payload)
        guard info.generation == Data(request.payload.dropFirst(4)) else { throw ReaderSessionError.wrongReader }
        return info
    }
}
extension AuthenticatedReaderSession {
    public func firmwareInfo(requestID: UInt32) async throws -> FirmwareReaderInfo {
        try Task.checkCancellation()
        let request = try FirmwareReaderInfo.request(generation: device.storageGeneration, requestID: requestID)
        let info = try FirmwareReaderInfo.decode(await exchange(request), request: request)
        _ = try info.refreshedDevice(device)
        return info
    }
}

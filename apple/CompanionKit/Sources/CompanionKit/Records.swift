import Foundation

public enum RecordKind: UInt8, Equatable, Sendable {
    case deviceDescriptor = 1, contentManifest, syncEvent, syncCheckpoint, transferState
}

public enum Board: UInt8, Equatable, Sendable { case x4 = 1, sticky, x4Pro, x4Classic, paperMono }
public enum TransferPhase: UInt8, Equatable, Sendable { case receiving = 1, verified, installing, committed, aborted }

public struct RecordEnvelope: Equatable, Sendable {
    public let kind: RecordKind
    public let bytes: Data
    public init(decoding bytes: Data) throws {
        guard bytes.count >= 2 else { throw ProtocolError.truncated }
        let start = bytes.startIndex
        guard bytes[start] == 1 else { throw ProtocolError.version }
        guard let kind = RecordKind(rawValue: bytes[start + 1]) else { throw ProtocolError.record }
        func byte(_ offset: Int) -> UInt8 { bytes[start + offset] }
        switch kind {
        case .deviceDescriptor:
            guard bytes.count == 74 else { throw ProtocolError.length }
            guard Board(rawValue: byte(34)) != nil, byte(39) <= 100, byte(40) > 0, byte(41) >= byte(40) else { throw ProtocolError.value }
        case .contentManifest:
            guard bytes.count == 63 else { throw ProtocolError.length }
            guard (1 ... 5).contains(byte(34)) else { throw ProtocolError.value }
        case .syncEvent:
            guard bytes.count >= 165, bytes.count <= 293 else { throw ProtocolError.length }
            guard byte(164) <= 4, bytes.count == 165 + 32 * Int(byte(164)),
                  (1 ... 10).contains(byte(50)), byte(127) <= 2 else { throw ProtocolError.value }
        case .syncCheckpoint:
            guard bytes.count == 98 else { throw ProtocolError.length }
        case .transferState:
            guard bytes.count == 99 else { throw ProtocolError.length }
            var reader = ByteReader(bytes)
            _ = try reader.take(82)
            let length = try reader.number(8)
            let offset = try reader.number(8)
            guard let phase = TransferPhase(rawValue: byte(98)), offset <= length,
                  phase == .receiving || phase == .aborted || offset == length else { throw ProtocolError.value }
        }
        self.kind = kind
        self.bytes = Data(bytes)
    }
}

public struct DeviceDescriptor: Equatable, Sendable {
    public let identity: Data
    public let storageGeneration: Data
    public let board: Board
    public let capabilities: UInt32
    public let batteryPercent: UInt8
    public let minimumProtocol: UInt8
    public let maximumProtocol: UInt8
    public let runningBuild: Data
    public var encoded: Data {
        var bytes = Data([1, 1]); bytes.reserveCapacity(74)
        bytes.append(identity); bytes.append(storageGeneration); bytes.append(board.rawValue)
        bytes.appendLittleEndian(UInt64(capabilities), count: 4)
        bytes.append(contentsOf: [batteryPercent, minimumProtocol, maximumProtocol]); bytes.append(runningBuild)
        return bytes
    }
    public init(decoding bytes: Data) throws {
        guard try RecordEnvelope(decoding: bytes).kind == .deviceDescriptor else { throw ProtocolError.record }
        var reader = ByteReader(bytes)
        _ = try reader.take(2)
        identity = try reader.take(16)
        storageGeneration = try reader.take(16)
        guard let board = Board(rawValue: UInt8(try reader.number(1))) else { throw ProtocolError.value }
        self.board = board
        capabilities = UInt32(try reader.number(4))
        batteryPercent = UInt8(try reader.number(1))
        minimumProtocol = UInt8(try reader.number(1))
        maximumProtocol = UInt8(try reader.number(1))
        runningBuild = try reader.take(32)
    }
}

public struct TransferState: Equatable, Sendable {
    public let transaction: Data
    public let owner: Data
    public let storageGeneration: Data
    public let contentHash: Data
    public let length: UInt64
    public let durableOffset: UInt64
    public let phase: TransferPhase
    public init(transaction: Data, owner: Data, storageGeneration: Data, contentHash: Data,
                length: UInt64, durableOffset: UInt64 = 0, phase: TransferPhase = .receiving) throws {
        guard [transaction, owner, storageGeneration].allSatisfy({ $0.count == 16 && $0.contains(where: { $0 != 0 }) }),
              contentHash.count == 32, durableOffset <= length,
              phase == .receiving || phase == .aborted || durableOffset == length else { throw ProtocolError.value }
        self.transaction = transaction; self.owner = owner; self.storageGeneration = storageGeneration
        self.contentHash = contentHash; self.length = length; self.durableOffset = durableOffset; self.phase = phase
    }
    public func encoded() -> Data {
        var bytes = Data([1, RecordKind.transferState.rawValue]); bytes.reserveCapacity(99)
        bytes.append(transaction); bytes.append(owner); bytes.append(storageGeneration); bytes.append(contentHash)
        bytes.appendLittleEndian(length, count: 8); bytes.appendLittleEndian(durableOffset, count: 8)
        bytes.append(phase.rawValue)
        return bytes
    }
    public init(decoding bytes: Data) throws {
        guard try RecordEnvelope(decoding: bytes).kind == .transferState else { throw ProtocolError.record }
        var reader = ByteReader(bytes)
        _ = try reader.take(2)
        transaction = try reader.take(16)
        owner = try reader.take(16)
        storageGeneration = try reader.take(16)
        contentHash = try reader.take(32)
        length = try reader.number(8)
        durableOffset = try reader.number(8)
        guard let phase = TransferPhase(rawValue: UInt8(try reader.number(1))) else { throw ProtocolError.value }
        self.phase = phase
        guard [transaction, owner, storageGeneration].allSatisfy({ $0.contains(where: { $0 != 0 }) }) else {
            throw ProtocolError.value
        }
    }
}

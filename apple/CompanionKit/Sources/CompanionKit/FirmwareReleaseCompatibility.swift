import Foundation

/// Release metadata admission only. Image integrity and reader-side validation remain mandatory.
public struct FirmwareReleaseCompatibility: Codable, Equatable, Sendable {
    public struct Range: Codable, Equatable, Sendable {
        public let minimum: UInt32
        public let maximum: UInt32
    }
    public let sha256: Data
    public let length: UInt64
    public let boardTags: [String]
    public let chipID: UInt16
    public let otaPartitionBytes: UInt64
    public let minimumBatteryPercent: UInt8
    public let stateSchema: Range
    public let companionProtocol: Range?
    public let initialUpgradeRequired: Bool
    public let journals: FirmwareJournalCompatibility

    private enum CodingKeys: String, CodingKey {
        case kind, sha256, length, boardTags, chipId, otaPartitionBytes, minimumBatteryPercent
        case stateSchema, companionProtocol, initialUpgradeRequired, supportedJournalHeaderVersions
    }
    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        guard try values.decode(String.self, forKey: .kind) == "firmware" else { throw ProtocolError.value }
        let hex = Array(try values.decode(String.self, forKey: .sha256).utf8)
        guard hex.count == 64, hex.allSatisfy({ (48 ... 57).contains($0) || (97 ... 102).contains($0) }) else {
            throw ProtocolError.value
        }
        var hash = Data(); hash.reserveCapacity(32)
        func nibble(_ byte: UInt8) -> UInt8 { byte <= 57 ? byte - 48 : byte - 87 }
        for at in stride(from: 0, to: hex.count, by: 2) { hash.append(nibble(hex[at]) << 4 | nibble(hex[at + 1])) }
        guard hash.contains(where: { $0 != 0 }) else { throw ProtocolError.value }
        sha256 = hash
        length = try values.decode(UInt64.self, forKey: .length)
        boardTags = try values.decode([String].self, forKey: .boardTags)
        chipID = try values.decode(UInt16.self, forKey: .chipId)
        otaPartitionBytes = try values.decode(UInt64.self, forKey: .otaPartitionBytes)
        minimumBatteryPercent = try values.decode(UInt8.self, forKey: .minimumBatteryPercent)
        stateSchema = try values.decode(Range.self, forKey: .stateSchema)
        companionProtocol = try values.decodeIfPresent(Range.self, forKey: .companionProtocol)
        initialUpgradeRequired = try values.decode(Bool.self, forKey: .initialUpgradeRequired)
        journals = try FirmwareJournalCompatibility(from: decoder)
        guard length >= 24, length <= otaPartitionBytes, otaPartitionBytes > 0,
              (30 ... 100).contains(minimumBatteryPercent), stateSchema.minimum <= stateSchema.maximum,
              boardTags.count == 1, ["x4", "sticky", "x4pro", "x4c", "papermono"].contains(boardTags[0]),
              chipID == (boardTags[0] == "x4" ? 5 : 9) else { throw ProtocolError.value }
        if let companionProtocol {
            guard !initialUpgradeRequired, companionProtocol.minimum > 0,
                  companionProtocol.minimum <= companionProtocol.maximum, companionProtocol.maximum <= 255 else {
                throw ProtocolError.value
            }
        } else if !initialUpgradeRequired { throw ProtocolError.value }
    }

    public func encode(to encoder: Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode("firmware", forKey: .kind)
        try values.encode(sha256.map { String(format: "%02x", $0) }.joined(), forKey: .sha256)
        try values.encode(length, forKey: .length)
        try values.encode(boardTags, forKey: .boardTags)
        try values.encode(chipID, forKey: .chipId)
        try values.encode(otaPartitionBytes, forKey: .otaPartitionBytes)
        try values.encode(minimumBatteryPercent, forKey: .minimumBatteryPercent)
        try values.encode(stateSchema, forKey: .stateSchema)
        try values.encode(companionProtocol, forKey: .companionProtocol)
        try values.encode(initialUpgradeRequired, forKey: .initialUpgradeRequired)
        try values.encode(journals.supportedJournalHeaderVersions, forKey: .supportedJournalHeaderVersions)
    }

    public func permits(device: DeviceDescriptor, readerStateSchema: UInt32?,
                        readerPartitionBytes: UInt64?, readerHeaderVersions: [UInt8]?) -> Bool {
        guard !initialUpgradeRequired, let protocolRange = companionProtocol,
              protocolRange.minimum <= 1, protocolRange.maximum >= 1,
              device.minimumProtocol <= 1, device.maximumProtocol >= 1,
              let schema = readerStateSchema, schema >= stateSchema.minimum, schema <= stateSchema.maximum,
              let partition = readerPartitionBytes, partition == otaPartitionBytes, length <= partition,
              device.batteryPercent <= 100, device.batteryPercent >= minimumBatteryPercent,
              journals.permits(readerHeaderVersions: readerHeaderVersions) else { return false }
        let tag: String
        switch device.board {
        case .x4: tag = "x4"
        case .sticky: tag = "sticky"
        case .x4Pro: tag = "x4pro"
        case .x4Classic: tag = "x4c"
        case .paperMono: tag = "papermono"
        }
        return boardTags == [tag]
    }
}

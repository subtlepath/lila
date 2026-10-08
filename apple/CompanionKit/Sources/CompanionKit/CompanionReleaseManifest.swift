import Foundation

public struct CompanionReleaseManifest: Sendable {
    public struct FirmwareAsset: Sendable {
        public let name: String
        public let url: URL
        public let metadata: FirmwareReleaseCompatibility
    }
    public let version: String
    public let tag: String
    public let revision: String
    public let channel: String
    public let firmware: [FirmwareAsset]

    private struct Raw: Decodable {
        let schemaVersion: UInt8
        let version: String, tag: String, revision: String, channel: String
        let assets: [Asset]
    }
    private struct Asset: Decodable {
        let name: String
        let url: URL
        let firmware: FirmwareReleaseCompatibility?
        private enum CodingKeys: String, CodingKey { case kind, name, url }
        init(from decoder: Decoder) throws {
            let fields = try decoder.container(keyedBy: CodingKeys.self)
            name = try fields.decode(String.self, forKey: .name)
            url = try fields.decode(URL.self, forKey: .url)
            switch try fields.decode(String.self, forKey: .kind) {
            case "firmware": firmware = try FirmwareReleaseCompatibility(from: decoder)
            case "course": firmware = nil
            default: throw ProtocolError.value
            }
        }
    }
    public static func decode(_ bytes: Data, repository: String = "subtlepath/lila",
                              allowReleaseCandidates: Bool = false) throws -> Self {
        guard bytes.count <= 262144,
              repository.range(of: "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$", options: .regularExpression) != nil else {
            throw ProtocolError.value
        }
        let raw = try JSONDecoder().decode(Raw.self, from: bytes)
        guard raw.schemaVersion == 1, raw.version.count <= 64,
              raw.version.range(of: "^[0-9]+\\.[0-9]+\\.[0-9]+(?:rc[0-9]*)?$", options: .regularExpression) != nil,
              raw.tag == "v" + raw.version, raw.channel == (raw.version.contains("rc") ? "rc" : "stable"),
              raw.channel == "stable" || allowReleaseCandidates,
              [40, 64].contains(raw.revision.utf8.count),
              raw.revision.utf8.allSatisfy({ (48 ... 57).contains($0) || (97 ... 102).contains($0) }),
              (1 ... 16).contains(raw.assets.count) else { throw ProtocolError.value }
        var firmware: [FirmwareAsset] = []; firmware.reserveCapacity(5)
        var names = Set<String>(), boards = Set<String>()
        for asset in raw.assets {
            guard asset.name.utf8.count <= 128, !asset.name.hasPrefix("."),
                  asset.name.range(of: "^[A-Za-z0-9_.-]+$", options: .regularExpression) != nil,
                  names.insert(asset.name).inserted,
                  asset.url.scheme == "https", asset.url.host == "github.com", asset.url.port == nil,
                  asset.url.user == nil, asset.url.password == nil, asset.url.query == nil, asset.url.fragment == nil,
                  asset.url.path == "/\(repository)/releases/download/\(raw.tag)/\(asset.name)" else {
                throw ProtocolError.value
            }
            if let metadata = asset.firmware {
                guard boards.insert(metadata.boardTags[0]).inserted else { throw ProtocolError.value }
                firmware.append(FirmwareAsset(name: asset.name, url: asset.url, metadata: metadata))
            }
        }
        guard boards == Set(["x4", "sticky", "x4pro", "x4c", "papermono"]) else { throw ProtocolError.value }
        return Self(version: raw.version, tag: raw.tag, revision: raw.revision, channel: raw.channel, firmware: firmware)
    }
    public func firmware(for board: Board) -> FirmwareAsset? {
        let tag: String
        switch board {
        case .x4: tag = "x4"
        case .sticky: tag = "sticky"
        case .x4Pro: tag = "x4pro"
        case .x4Classic: tag = "x4c"
        case .paperMono: tag = "papermono"
        }
        return firmware.first { $0.metadata.boardTags == [tag] }
    }
}

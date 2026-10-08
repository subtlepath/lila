import Foundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif

public enum PreferenceKey: UInt8, CaseIterable, Sendable {
    case fontSelection = 1, fontPointSize, lineSpacing, alignment, paragraphSpacing, wordSpacing, characterSpacing
    case margin, hyphenation, language, dictionary
    case textAntiAliasing, embeddedStyle, focusReading
    case tintaNewPerDay = 32, tintaReviewCap, tintaRetentionPermille, tintaMaximumInterval, tintaSessionSize
    case tintaTextSize, tintaLanguage, tintaShowVulgar, tintaTypedAnswers
}
public struct ContentSelection: Equatable, Sendable {
    public let hash: Data
    public let name: String
    public init(hash: Data, name: String) throws {
        guard hash.count == 32, hash.contains(where: { $0 != 0 }), !name.isEmpty, name.utf8.count <= 31,
              name != ".", name != "..", !name.utf8.contains(where: { $0 < 32 || $0 == 127 || $0 == 47 || $0 == 92 || $0 == 58 }) else { throw ProtocolError.value }
        self.hash = hash; self.name = name
    }
}
public enum PreferenceValue: Equatable, Sendable {
    case integer(Int32), languageTag(String), content(ContentSelection?)
}
public struct PreferenceBody: Equatable, Sendable {
    public static let scope = Data(SHA256.hash(data: Data("lila:portable-preferences:v1".utf8)))
    public let key: PreferenceKey
    public let value: PreferenceValue
    public init(key: PreferenceKey, value: PreferenceValue) throws {
        guard Self.valid(key: key, value: value) else { throw ProtocolError.value }
        self.key = key; self.value = value
    }
    public var requiredContent: ContentSelection? { if case let .content(selection) = value { return selection }; return nil }
    private static func valid(key: PreferenceKey, value: PreferenceValue) -> Bool {
        if key == .fontSelection {
            if case let .integer(number) = value { return (0 ... 1).contains(number) }
            if case .content(.some) = value { return true }
            return false
        }
        if key == .dictionary { if case .content = value { return true }; return false }
        if key == .language {
            guard case let .languageTag(tag) = value, !tag.isEmpty, tag.utf8.count <= 63 else { return false }
            let parts = tag.split(separator: "-", omittingEmptySubsequences: false)
            return parts.enumerated().allSatisfy { index, part in
                (1 ... 8).contains(part.utf8.count) && part.utf8.allSatisfy {
                    (65 ... 90).contains($0) || (97 ... 122).contains($0) || (index > 0 && (48 ... 57).contains($0))
                }
            }
        }
        guard case let .integer(number) = value else { return false }
        switch key {
        case .fontPointSize: return (1 ... 255).contains(number)
        case .lineSpacing: return (0 ... 3).contains(number)
        case .alignment: return (0 ... 4).contains(number)
        case .paragraphSpacing, .hyphenation, .textAntiAliasing, .embeddedStyle, .focusReading,
             .tintaShowVulgar, .tintaTypedAnswers: return (0 ... 1).contains(number)
        case .wordSpacing: return (50 ... 200).contains(number) && number % 25 == 0
        case .characterSpacing: return (-2 ... 2).contains(number)
        case .margin: return (5 ... 40).contains(number) && number % 5 == 0
        case .tintaNewPerDay: return (0 ... 200).contains(number)
        case .tintaReviewCap: return (0 ... 9999).contains(number)
        case .tintaRetentionPermille: return (700 ... 970).contains(number)
        case .tintaMaximumInterval: return (1 ... 36500).contains(number)
        case .tintaSessionSize: return (5 ... 500).contains(number)
        case .tintaTextSize, .tintaLanguage: return (0 ... 2).contains(number)
        default: return false
        }
    }
    public var encoded: Data {
        var bytes = Data([1, SyncEventKind.preference.rawValue, key.rawValue]); bytes.reserveCapacity(69)
        switch value {
        case let .integer(number): bytes.append(1); bytes.appendLittleEndian(UInt64(UInt32(bitPattern: number)), count: 4)
        case let .languageTag(tag): bytes.append(2); bytes.append(UInt8(tag.utf8.count)); bytes.append(contentsOf: tag.utf8)
        case let .content(selection):
            bytes.append(3); bytes.append(selection == nil ? 0 : 1)
            if let selection { bytes.append(selection.hash); bytes.append(UInt8(selection.name.utf8.count)); bytes.append(contentsOf: selection.name.utf8) }
        }
        return bytes
    }
    public init(decoding bytes: Data) throws {
        guard (5 ... 69).contains(bytes.count) else { throw ProtocolError.length }
        var reader = ByteReader(bytes)
        guard try reader.number(1) == 1 else { throw ProtocolError.version }
        guard try reader.number(1) == UInt64(SyncEventKind.preference.rawValue),
              let key = PreferenceKey(rawValue: UInt8(try reader.number(1))) else { throw ProtocolError.value }
        let value: PreferenceValue
        switch try reader.number(1) {
        case 1: value = .integer(Int32(bitPattern: UInt32(try reader.number(4))))
        case 2:
            let count = Int(try reader.number(1))
            guard count <= 63, let tag = String(data: try reader.take(count), encoding: .utf8) else { throw ProtocolError.value }
            value = .languageTag(tag)
        case 3:
            let present = try reader.number(1); guard present <= 1 else { throw ProtocolError.value }
            if present == 0 { value = .content(nil) }
            else {
                let hash = try reader.take(32), count = Int(try reader.number(1))
                guard count <= 31, let name = String(data: try reader.take(count), encoding: .utf8) else { throw ProtocolError.value }
                value = .content(try ContentSelection(hash: hash, name: name))
            }
        default: throw ProtocolError.value
        }
        guard reader.position == bytes.count else { throw ProtocolError.length }
        try self.init(key: key, value: value)
    }
    public init(mutation: JournalMutation) throws {
        try self.init(decoding: mutation.body)
        guard mutation.event.kind == .preference, mutation.event.resource == Self.scope,
              mutation.event.schedulerVersion == 0, mutation.event.schedulerConfiguration == Data(count: 32) else { throw ProtocolError.value }
    }
}
public struct PreferenceCandidate: Equatable, Sendable {
    public let event: EventIdentity
    public let body: PreferenceBody
}
public struct PreferenceState: Equatable, Sendable {
    public let key: PreferenceKey
    public let candidates: [PreferenceCandidate]
    public var requiresResolution: Bool {
        guard let first = candidates.first else { return false }
        return candidates.contains { $0.body.value != first.body.value }
    }
    public var resolutionAncestors: [EventIdentity] { candidates.map(\.event) }
    public func missingContent(installed: Set<Data>) -> ContentSelection? {
        guard !requiresResolution, let selection = candidates.first?.body.requiredContent, !installed.contains(selection.hash) else { return nil }
        return selection
    }
}
public enum PreferenceHistory {
    public static func reconcile(_ deliveries: [JournalMutation]) throws -> [PreferenceState] {
        let history = try CausalHistory(events: deliveries.map(\.event))
        var groups: [PreferenceKey: [EventIdentity: PreferenceCandidate]] = [:]; groups.reserveCapacity(PreferenceKey.allCases.count)
        for mutation in deliveries where mutation.event.kind == .preference {
            let body = try PreferenceBody(mutation: mutation)
            groups[body.key, default: [:]][mutation.event.identity] = PreferenceCandidate(event: mutation.event.identity, body: body)
        }
        return try groups.keys.sorted(by: { $0.rawValue < $1.rawValue }).map { key in
            guard let group = groups[key] else { throw ProtocolError.record }
            let heads = try history.maximal(Array(group.keys))
            let candidates = try heads.map { event in
                guard let candidate = group[event] else { throw ProtocolError.record }; return candidate
            }
            return PreferenceState(key: key, candidates: candidates)
        }
    }
}

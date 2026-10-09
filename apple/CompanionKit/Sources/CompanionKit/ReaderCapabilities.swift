import Foundation

public struct ReaderCapabilities: OptionSet, Equatable, Sendable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }
    public static let declaredTransfers = ReaderCapabilities(rawValue: 1 << 0)
    public static let courseTransfers = ReaderCapabilities(rawValue: 1 << 1)
    public static let courseSwitches = ReaderCapabilities(rawValue: 1 << 4)
    public static let fontTransfers = ReaderCapabilities(rawValue: 1 << 5)
    public static let vectorFontTransfers = ReaderCapabilities(rawValue: 1 << 6)
    public static let dictionaryTransfers = ReaderCapabilities(rawValue: 1 << 7)
    public static let journalMergeReadiness = ReaderCapabilities(rawValue: 1 << 9)
    public static let epubRemovals = ReaderCapabilities(rawValue: 1 << 8)
    public static let fontRemovals = ReaderCapabilities(rawValue: 1 << 13)
    public static let dictionaryRemovals = ReaderCapabilities(rawValue: 1 << 14)
    public static let contentReads = ReaderCapabilities(rawValue: 1 << 10)
    public static let contentMetadata = ReaderCapabilities(rawValue: 1 << 11)
    public var supportsContentMetadata: Bool { contains([.contentReads, .contentMetadata]) }
    public static let wifiContentReads = ReaderCapabilities(rawValue: 1 << 12)
    public var supportsWifiContentRead: Bool { contains([.contentReads, .contentMetadata, .wifiContentReads]) }
    public var supportsContentRead: Bool { contains(.contentReads) }
    public var supportsEpubRemoval: Bool { contains(.epubRemovals) }
    public var supportsFontRemoval: Bool { contains(.fontRemovals) }
    public var supportsDictionaryRemoval: Bool { contains(.dictionaryRemovals) }
    public func supportsRemoval(of kind: ContentKind) -> Bool {
        switch kind {
        case .epub: return supportsEpubRemoval
        case .font: return supportsFontRemoval
        case .dictionary: return supportsDictionaryRemoval
        default: return false
        }
    }
    public var supportsDictionaryTransfer: Bool { contains([.declaredTransfers, .dictionaryTransfers]) }
    public var supportsCourseTransfer: Bool { contains([.declaredTransfers, .courseTransfers]) }
    public var supportsCourseSwitch: Bool { contains([.declaredTransfers, .courseTransfers, .courseSwitches]) }
}

extension DeviceDescriptor {
    public var readerCapabilities: ReaderCapabilities { ReaderCapabilities(rawValue: capabilities) }
}

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
    public var supportsEpubRemoval: Bool { contains(.epubRemovals) }
    public var supportsDictionaryTransfer: Bool { contains([.declaredTransfers, .dictionaryTransfers]) }
    public var supportsCourseTransfer: Bool { contains([.declaredTransfers, .courseTransfers]) }
}

extension DeviceDescriptor {
    public var readerCapabilities: ReaderCapabilities { ReaderCapabilities(rawValue: capabilities) }
}

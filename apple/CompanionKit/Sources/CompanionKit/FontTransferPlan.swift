import Foundation

public struct FontTransferPlan: Equatable, Sendable {
    public let formatVersion: UInt32
    public let destination: String
    public init(content: LibraryContent) throws {
        guard content.kind == .font else { throw TransferRunnerError.unsupportedContent }
        let filename = content.originalFilename.precomposedStringWithCanonicalMapping
        let suffix = (filename as NSString).pathExtension.lowercased()
        let base = (filename as NSString).deletingPathExtension
        guard !base.isEmpty, !base.hasPrefix("."), !base.hasPrefix("_"),
              !filename.utf8.contains(where: { $0 < 32 || $0 == 127 || $0 == 47 || $0 == 92 }) else {
            throw StoreError.invalidValue
        }
        if suffix == "cpfont" {
            guard let separator = base.lastIndex(of: "_"), separator != base.startIndex else { throw StoreError.invalidValue }
            let sizeText = base[base.index(after: separator)...]
            guard !sizeText.isEmpty, sizeText.utf8.allSatisfy({ (48...57).contains($0) }),
                  let size = UInt16(sizeText), (1...255).contains(size) else { throw StoreError.invalidValue }
            let family = String(base[..<separator])
            formatVersion = 4
            destination = "/fonts/\(family)/\(base).cpfont"
        } else {
            guard ["ttf", "otf", "ttc"].contains(suffix) else { throw TransferRunnerError.unsupportedContent }
            formatVersion = 1
            destination = "/fonts/\(base).\(suffix)"
        }
        guard destination.utf8.count < 128 else { throw StoreError.invalidValue }
    }
    public func admit(_ device: DeviceDescriptor) throws {
        guard device.readerCapabilities.contains([.declaredTransfers, .fontTransfers]),
              formatVersion != 1 || device.readerCapabilities.contains(.vectorFontTransfers) else {
            throw TransferRunnerError.unsupportedContent
        }
    }
}

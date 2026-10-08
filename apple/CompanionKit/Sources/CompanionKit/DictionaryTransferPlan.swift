import Foundation

public struct DictionaryTransferPlan: Equatable, Sendable {
    public let formatVersion: UInt32 = 1
    public let destination: String
    public init(content: LibraryContent) throws {
        try self.init(manifest: ContentManifest(content: content.id, kind: content.kind, length: content.length,
            formatVersion: 1, logicalIdentity: Data(count: 16)))
    }
    public init(manifest: ContentManifest) throws {
        guard manifest.kind == .dictionary, manifest.formatVersion == 1,
              (22...UInt64(UInt32.max)).contains(manifest.length),
              manifest.logicalIdentity == Data(count: 16), manifest.content.digest.contains(where: { $0 != 0 }) else {
            throw TransferRunnerError.unsupportedContent
        }
        destination = "/dictionaries/\(manifest.content.hex)/dictionary"
    }
    public func admit(_ device: DeviceDescriptor) throws {
        guard device.readerCapabilities.supportsDictionaryTransfer else { throw TransferRunnerError.unsupportedContent }
    }
}

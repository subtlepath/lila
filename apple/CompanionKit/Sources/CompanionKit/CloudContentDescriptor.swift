import Foundation

public struct CloudContentDescriptor: Codable, Equatable, Sendable {
    public let version: Int
    public let id: ContentID
    public let kind: ContentKind
    public let length: UInt64
    public let originalFilename: String
    public init(content: LibraryContent) throws {
        version = 1; id = content.id; kind = content.kind; length = content.length
        originalFilename = content.originalFilename
        try validate()
    }
    public func validate() throws {
        guard version == 1, length > 0, length <= UInt64(Int64.max),
              !originalFilename.isEmpty, originalFilename.utf8.count <= 255,
              !originalFilename.contains("/"), !originalFilename.contains("\\"), !originalFilename.utf8.contains(0) else {
            throw StoreError.invalidValue
        }
        let suffix = (originalFilename as NSString).pathExtension.lowercased()
        let supported: Bool
        switch kind {
        case .epub: supported = suffix == "epub"
        case .course: supported = suffix == "pack"
        case .font: supported = ["cpfont", "ttf", "otf"].contains(suffix)
        case .dictionary: supported = suffix == "zip"
        case .firmware: supported = false
        }
        guard supported else { throw ImportError.unsupportedEntry }
    }
}

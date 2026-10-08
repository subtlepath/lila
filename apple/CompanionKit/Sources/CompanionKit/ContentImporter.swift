import Foundation

public struct ImportedEPUB: Sendable {
    public let content: LibraryContent
    public let metadata: EpubMetadata
}
public struct ImportedCoursePack: Sendable {
    public let content: LibraryContent
    public let metadata: CoursePackMetadata
}
public struct ImportedDictionary: Sendable {
    public let content: LibraryContent
    public let metadata: DictionaryBundleMetadata
}

// Validation and durable file publication precede SQLite metadata commits.
public actor ContentImporter {
    private let vault: ContentVault
    private let library: LibraryStore
    public init(vault: ContentVault, library: LibraryStore) {
        self.vault = vault; self.library = library
    }
    public func importFile(_ source: URL) async throws -> LibraryContent {
        guard source.isFileURL else { throw ImportError.unsupportedEntry }
        switch source.pathExtension.lowercased() {
        case "epub": return try await importEPUB(source).content
        case "pack": return try await importCoursePack(source).content
        case "cpfont": return try await importBitmapFont(source)
        case "ttf", "otf": return try await importVectorFont(source)
        case "zip": return try await importDictionaryBundle(source).content
        default: throw ImportError.unsupportedEntry
        }
    }
    public func importFirmware(_ source: URL, releaseAsset: Data) async throws -> LibraryContent {
        guard releaseAsset.count <= 16384 else { throw StoreError.invalidValue }
        let compatibility = try JSONDecoder().decode(FirmwareReleaseCompatibility.self, from: releaseAsset)
        return try await importFirmware(source, compatibility: compatibility, releaseAsset: releaseAsset,
                                        filename: source.lastPathComponent)
    }
    public func importFirmware(_ source: URL, asset: CompanionReleaseManifest.FirmwareAsset) async throws -> LibraryContent {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        return try await importFirmware(source, compatibility: asset.metadata,
                                        releaseAsset: encoder.encode(asset.metadata), filename: asset.name)
    }
    private func importFirmware(_ source: URL, compatibility: FirmwareReleaseCompatibility,
                                releaseAsset: Data, filename: String) async throws -> LibraryContent {
        let object = try await vault.importFirmware(source, compatibility: compatibility)
        try Task.checkCancellation()
        let content = LibraryContent(id: object.id, kind: .firmware, length: object.length,
                                     title: URL(fileURLWithPath: filename).deletingPathExtension().lastPathComponent,
                                     originalFilename: filename)
        try await library.putFirmware(content, releaseAsset: releaseAsset)
        return content
    }
    public func importEPUB(_ source: URL) async throws -> ImportedEPUB {
        let (object, metadata) = try await vault.importValidatedFile(source) { url in
            try EpubInspector.inspect(url)
        }
        try Task.checkCancellation()
        let content = LibraryContent(id: object.id, kind: .epub, length: object.length,
                                     title: metadata.title, originalFilename: source.lastPathComponent,
                                     authors: metadata.authors, identifiers: metadata.identifiers, languages: metadata.languages)
        try await library.put(content)
        return ImportedEPUB(content: content, metadata: metadata)
    }
    public func importCoursePack(_ source: URL) async throws -> ImportedCoursePack {
        let (object, metadata) = try await vault.importValidatedFile(source) { url in
            try CoursePackInspector.inspect(url)
        }
        try Task.checkCancellation()
        let content = LibraryContent(id: object.id, kind: .course, length: object.length,
                                     title: source.deletingPathExtension().lastPathComponent,
                                     originalFilename: source.lastPathComponent,
                                     languages: [metadata.locale])
        try await library.putCoursePack(content, metadata: metadata)
        return ImportedCoursePack(content: content, metadata: metadata)
    }
    public func importBitmapFont(_ source: URL) async throws -> LibraryContent {
        let (object, _) = try await vault.importValidatedFile(source) { url in
            try BitmapFontInspector.inspect(url)
        }
        try Task.checkCancellation()
        let content = LibraryContent(id: object.id, kind: .font, length: object.length,
                                     title: source.deletingPathExtension().lastPathComponent,
                                     originalFilename: source.lastPathComponent)
        try await library.put(content)
        return content
    }
    public func importVectorFont(_ source: URL) async throws -> LibraryContent {
        let (object, _) = try await vault.importValidatedFile(source) { url in
            try VectorFontInspector.inspect(url)
        }
        try Task.checkCancellation()
        let content = LibraryContent(id: object.id, kind: .font, length: object.length,
                                     title: source.deletingPathExtension().lastPathComponent,
                                     originalFilename: source.lastPathComponent)
        try await library.put(content)
        return content
    }
    public func importDictionaryBundle(_ source: URL) async throws -> ImportedDictionary {
        let (object, metadata) = try await vault.importValidatedFile(source) { url in
            try DictionaryBundleInspector.inspect(url, scratchDirectory: url.deletingLastPathComponent())
        }
        try Task.checkCancellation()
        let content = LibraryContent(id: object.id, kind: .dictionary, length: object.length,
                                     title: metadata.info.name, originalFilename: source.lastPathComponent)
        try await library.put(content)
        return ImportedDictionary(content: content, metadata: metadata)
    }
}

public extension ContentImporter {
    func importCloudAsset(_ source: URL, descriptor: CloudContentDescriptor) async throws -> LibraryContent {
        try descriptor.validate()
        let (object, metadata) = try await vault.importValidatedFile(source, expectedID: descriptor.id,
            expectedLength: descriptor.length) { url -> CloudAssetMetadata in
            switch descriptor.kind {
            case .epub: return .epub(try EpubInspector.inspect(url))
            case .course: return .course(try CoursePackInspector.inspect(url))
            case .font:
                if (descriptor.originalFilename as NSString).pathExtension.lowercased() == "cpfont" {
                    _ = try BitmapFontInspector.inspect(url)
                } else { _ = try VectorFontInspector.inspect(url) }
                return .plain(nil)
            case .dictionary:
                return .plain(try DictionaryBundleInspector.inspect(url, scratchDirectory: url.deletingLastPathComponent()).info.name)
            case .firmware: throw ImportError.unsupportedEntry
            }
        }
        try Task.checkCancellation()
        let basename = (descriptor.originalFilename as NSString).deletingPathExtension
        let content: LibraryContent
        switch metadata {
        case .epub(let epub):
            content = LibraryContent(id: object.id, kind: descriptor.kind, length: object.length,
                title: epub.title, originalFilename: descriptor.originalFilename,
                authors: epub.authors, identifiers: epub.identifiers, languages: epub.languages)
        case .course(let course):
            content = LibraryContent(id: object.id, kind: descriptor.kind, length: object.length,
                title: basename, originalFilename: descriptor.originalFilename, languages: [course.locale])
            try await library.putCoursePack(content, metadata: course)
            return content
        case .plain(let title):
            content = LibraryContent(id: object.id, kind: descriptor.kind, length: object.length,
                title: title ?? basename, originalFilename: descriptor.originalFilename)
        }
        try await library.put(content)
        return content
    }
}
private enum CloudAssetMetadata: Sendable {
    case epub(EpubMetadata), course(CoursePackMetadata), plain(String?)
}

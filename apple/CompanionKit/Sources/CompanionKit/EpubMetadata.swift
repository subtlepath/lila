import Foundation
#if canImport(FoundationXML)
import FoundationXML
#endif
import ZIPFoundation

public struct EpubMetadata: Equatable, Sendable {
    public let title: String
    public let authors: [String]
    public let identifiers: [String]
    public let languages: [String]
    public let packagePath: String
}

public enum EpubInspector {
    public static func inspect(_ url: URL) throws -> EpubMetadata {
        let entries = try ArchiveValidator.entries(url)
        let archive = try Archive(url: url, accessMode: .read)
        guard try read("mimetype", from: archive, entries: entries, limit: 64) == Data("application/epub+zip".utf8) else {
            throw ImportError.integrity
        }
        let container = try parse(read("META-INF/container.xml", from: archive, entries: entries, limit: 1024 * 1024))
        guard container.name == "container", container.namespace == "urn:oasis:names:tc:opendocument:xmlns:container",
              let roots = container.children.first(where: { $0.name == "rootfiles" && $0.namespace == container.namespace }),
              let root = roots.children.first(where: {
                  $0.name == "rootfile" && $0.namespace == container.namespace &&
                  $0.attributes["media-type"] == "application/oebps-package+xml"
              }), let rawPath = root.attributes["full-path"] else { throw ImportError.integrity }
        let path = try ArchiveValidator.canonicalPath(rawPath, directory: false)
        let package = try parse(read(path, from: archive, entries: entries, limit: 4 * 1024 * 1024))
        let opf = "http://www.idpf.org/2007/opf"
        guard package.name == "package", package.namespace == opf,
              let metadata = package.children.first(where: { $0.name == "metadata" && $0.namespace == opf }) else {
            throw ImportError.integrity
        }
        func values(_ name: String) -> [String] {
            metadata.children.filter { $0.name == name && $0.namespace == "http://purl.org/dc/elements/1.1/" }
                .map { $0.text.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
        }
        guard let title = values("title").first else { throw ImportError.integrity }
        try validatePackage(package, path: path, entries: entries)
        return EpubMetadata(title: title, authors: values("creator"), identifiers: values("identifier"),
                            languages: values("language"), packagePath: path)
    }

    private struct ManifestItem {
        let mediaType: String
        let local: Bool
        let fallback: String?
    }

    private static func validatePackage(_ package: XMLNode, path: String, entries: [String: Entry]) throws {
        let namespace = "http://www.idpf.org/2007/opf"
        guard let manifest = package.children.first(where: { $0.name == "manifest" && $0.namespace == namespace }),
              let spine = package.children.first(where: { $0.name == "spine" && $0.namespace == namespace }) else {
            throw ImportError.integrity
        }
        var items: [String: ManifestItem] = [:]
        items.reserveCapacity(manifest.children.count)
        var resources = Set<String>()
        resources.reserveCapacity(manifest.children.count)
        for item in manifest.children where item.name == "item" && item.namespace == namespace {
            try Task.checkCancellation()
            guard let id = item.attributes["id"], !id.isEmpty, items[id] == nil,
                  let href = item.attributes["href"], !href.isEmpty,
                  let media = item.attributes["media-type"], !media.isEmpty else { throw ImportError.integrity }
            let resource = try resourcePath(href, packagePath: path)
            if let resource {
                guard resources.insert(resource).inserted else { throw ImportError.integrity }
                guard let entry = entries[resource], entry.type == .file else { throw ImportError.missingEntry }
            }
            items[id] = ManifestItem(mediaType: media, local: resource != nil, fallback: item.attributes["fallback"])
        }
        guard !items.isEmpty else { throw ImportError.integrity }
        // Cache fallback results so shared chains are walked only once.
        var supported: [String: Bool] = [:]
        supported.reserveCapacity(items.count)
        for id in items.keys {
            var visited = Set<String>()
            var chain: [String] = []
            var current: String? = id
            while let candidate = current, supported[candidate] == nil {
                try Task.checkCancellation()
                guard visited.insert(candidate).inserted, let item = items[candidate] else { throw ImportError.integrity }
                chain.append(candidate)
                current = item.fallback
            }
            var readable = current.flatMap { supported[$0] } ?? false
            for candidate in chain.reversed() {
                guard let item = items[candidate] else { throw ImportError.integrity }
                readable = readable || (item.local && ["application/xhtml+xml", "image/svg+xml"].contains(item.mediaType))
                supported[candidate] = readable
            }
        }
        var count = 0
        for reference in spine.children where reference.name == "itemref" && reference.namespace == namespace {
            guard let id = reference.attributes["idref"] else { throw ImportError.integrity }
            guard let readable = supported[id] else { throw ImportError.integrity }
            guard readable else { throw ImportError.unsupportedEntry }
            count += 1
        }
        guard count > 0 else { throw ImportError.integrity }
    }

    private static func resourcePath(_ href: String, packagePath: String) throws -> String? {
        guard let components = URLComponents(string: href), components.query == nil else { throw ImportError.unsafePath }
        if let scheme = components.scheme {
            guard ["http", "https"].contains(scheme.lowercased()), components.host != nil else { throw ImportError.unsafePath }
            return nil
        }
        guard components.host == nil, let decoded = components.percentEncodedPath.removingPercentEncoding,
              !decoded.isEmpty, !decoded.hasPrefix("/"), !decoded.contains("\\"), !decoded.contains(":") else {
            throw ImportError.unsafePath
        }
        var segments = packagePath.split(separator: "/").dropLast().map(String.init)
        for segment in decoded.split(separator: "/", omittingEmptySubsequences: false) {
            if segment == "." { continue }
            if segment == ".." {
                guard !segments.isEmpty else { throw ImportError.unsafePath }
                segments.removeLast()
            } else {
                guard !segment.isEmpty else { throw ImportError.unsafePath }
                segments.append(String(segment))
            }
        }
        let resolved = segments.joined(separator: "/")
        return try ArchiveValidator.canonicalPath(resolved, directory: false)
    }

    private static func read(_ path: String, from archive: Archive, entries: [String: Entry], limit: UInt64) throws -> Data {
        guard let entry = entries[path], entry.type == .file else { throw ImportError.missingEntry }
        guard entry.uncompressedSize <= limit else { throw ImportError.resourceLimit }
        var data = Data()
        data.reserveCapacity(Int(entry.uncompressedSize))
        let checksum = try archive.extract(entry, bufferSize: 64 * 1024) { chunk in
            try Task.checkCancellation()
            guard UInt64(data.count) <= limit, UInt64(chunk.count) <= limit - UInt64(data.count) else {
                throw ImportError.resourceLimit
            }
            data.append(chunk)
        }
        guard UInt64(data.count) == entry.uncompressedSize, checksum == entry.checksum else { throw ImportError.integrity }
        return data
    }

    private static func parse(_ data: Data) throws -> XMLNode {
        // Inspect ASCII markup in UTF-8/16/32 before platform XML parsers process declarations.
        let markup = String(decoding: data.filter { $0 != 0 }, as: UTF8.self)
            .replacingOccurrences(of: "<!--[\\s\\S]*?-->|<!\\[CDATA\\[[\\s\\S]*?\\]\\]>",
                                  with: "", options: .regularExpression)
        guard !markup.contains("<!DOCTYPE"), !markup.contains("<!ENTITY") else { throw ImportError.integrity }
        let delegate = MetadataXMLParser()
        let parser = XMLParser(data: data)
        parser.shouldProcessNamespaces = true
        parser.shouldResolveExternalEntities = false
        parser.delegate = delegate
        guard parser.parse(), !delegate.rejected, let root = delegate.root, delegate.stack.isEmpty else {
            throw ImportError.integrity
        }
        return root
    }
}

private final class XMLNode {
    let name: String
    let namespace: String
    let attributes: [String: String]
    var children: [XMLNode] = []
    var text = ""
    init(name: String, namespace: String, attributes: [String: String]) {
        self.name = name; self.namespace = namespace; self.attributes = attributes
    }
}

private final class MetadataXMLParser: NSObject, XMLParserDelegate {
    var root: XMLNode?
    var stack: [XMLNode] = []
    var rejected = false
    private var nodes = 0
    private func reject(_ parser: XMLParser) { rejected = true; parser.abortParsing() }
    func parser(_ parser: XMLParser, didStartElement name: String, namespaceURI: String?,
                qualifiedName: String?, attributes: [String: String]) {
        guard stack.count < 32, nodes < 20_000 else { reject(parser); return }
        nodes += 1
        let node = XMLNode(name: name, namespace: namespaceURI ?? "", attributes: attributes)
        if let parent = stack.last { parent.children.append(node) }
        else if root == nil { root = node } else { reject(parser); return }
        stack.append(node)
    }
    func parser(_ parser: XMLParser, didEndElement name: String, namespaceURI: String?, qualifiedName: String?) {
        guard let node = stack.popLast(), node.name == name else { reject(parser); return }
        if let parent = stack.last { parent.text += node.text }
    }
    func parser(_ parser: XMLParser, foundCharacters string: String) { stack.last?.text += string }
    func parser(_ parser: XMLParser, foundCDATA data: Data) {
        guard let text = String(data: data, encoding: .utf8) else { reject(parser); return }
        stack.last?.text += text
    }
    func parser(_ parser: XMLParser, foundInternalEntityDeclarationWithName name: String, value: String?) { reject(parser) }
    func parser(_ parser: XMLParser, foundExternalEntityDeclarationWithName name: String, publicID: String?, systemID: String?) {
        reject(parser)
    }
    func parser(_ parser: XMLParser, resolveExternalEntityName name: String, systemID: String?) -> Data? {
        reject(parser); return nil
    }
}

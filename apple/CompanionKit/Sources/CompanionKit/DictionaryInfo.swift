import Foundation

public struct DictionaryInfo: Equatable, Sendable {
    public let name: String
    public let version: String
    public let wordCount: UInt32
    public let indexBytes: UInt32
    public let synonymCount: UInt32?
    public let sameTypeSequence: String?
    public var htmlDefinitions: Bool { sameTypeSequence == "h" }

    public init(data: Data) throws {
        guard data.count <= 64 * 1024 else { throw ImportError.resourceLimit }
        guard !data.contains(0), String(data: data, encoding: .utf8) != nil else { throw ImportError.integrity }
        let lines = data.split(separator: 10, omittingEmptySubsequences: false).map { String(decoding: $0, as: UTF8.self) }
        guard lines.first?.trimmingCharacters(in: .newlines) == "StarDict's dict ifo file" else { throw ImportError.integrity }
        var fields: [Data: String] = [:]
        fields.reserveCapacity(16)
        var offset = (lines.first?.utf8.count ?? 0) + 1
        for raw in lines.dropFirst() {
            defer { offset += raw.utf8.count + 1 }
            let line = raw.hasSuffix("\r") ? String(raw.dropLast()) : raw
            if line.isEmpty { continue }
            guard let split = line.firstIndex(of: "=") else { throw ImportError.integrity }
            let key = String(line[..<split]), value = String(line[line.index(after: split)...])
            guard !key.isEmpty, fields[Data(key.utf8)] == nil else { throw ImportError.integrity }
            if key == "idxoffsetbits" || key == "sametypesequence" {
                guard offset + line.utf8.count < 2047 else { throw ImportError.unsupportedEntry }
            }
            fields[Data(key.utf8)] = value
        }
        let readerPrefix = String(decoding: data.prefix(2047), as: UTF8.self)
        for key in ["idxoffsetbits", "sametypesequence"] {
            guard readerPrefix.components(separatedBy: key).count == (fields[Data(key.utf8)] == nil ? 1 : 2) else {
                throw ImportError.unsupportedEntry
            }
        }
        guard let version = fields[Data("version".utf8)], ["2.4.2", "3.0.0"].contains(version) else { throw ImportError.unsupportedEntry }
        guard fields[Data("idxoffsetbits".utf8)] == nil || fields[Data("idxoffsetbits".utf8)] == "32" else { throw ImportError.unsupportedEntry }
        guard let name = fields[Data("bookname".utf8)], !name.trimmingCharacters(in: .whitespaces).isEmpty,
              let countText = fields[Data("wordcount".utf8)], let wordCount = Self.decimal(countText),
              let sizeText = fields[Data("idxfilesize".utf8)], let indexBytes = Self.decimal(sizeText) else { throw ImportError.integrity }
        let synonyms: UInt32?
        if let value = fields[Data("synwordcount".utf8)] {
            guard let count = Self.decimal(value) else { throw ImportError.integrity }
            synonyms = count
        } else { synonyms = nil }
        let sequence = fields[Data("sametypesequence".utf8)]
        if let sequence {
            guard !sequence.isEmpty, sequence.utf8.allSatisfy({ (65 ... 90).contains($0) || (97 ... 122).contains($0) }) else {
                throw ImportError.integrity
            }
        }
        self.name = name; self.version = version; self.wordCount = wordCount; self.indexBytes = indexBytes
        synonymCount = synonyms; sameTypeSequence = sequence
    }
    private static func decimal(_ value: String) -> UInt32? {
        guard !value.isEmpty, value.utf8.allSatisfy({ (48 ... 57).contains($0) }) else { return nil }
        return UInt32(value)
    }
}

import Foundation

public enum DictionaryIndexInspector {
    public static func inspect(_ url: URL, info: DictionaryInfo, definitionBytes: UInt64) throws {
        var validator = DictionaryIndexValidator(words: info.wordCount, bytes: UInt64(info.indexBytes),
                                                 mode: .definitions(length: definitionBytes))
        try stream(url, validator: &validator)
    }
    public static func inspectSynonyms(_ url: URL, info: DictionaryInfo) throws {
        guard let count = info.synonymCount else { throw ImportError.integrity }
        let size = try FileManager.default.attributesOfItem(atPath: url.path)[.size] as? NSNumber
        guard let size else { throw ImportError.integrity }
        var validator = DictionaryIndexValidator(words: count, bytes: size.uint64Value, mode: .synonyms(words: info.wordCount))
        try stream(url, validator: &validator)
    }
    private static func stream(_ url: URL, validator: inout DictionaryIndexValidator) throws {
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        while let chunk = try file.read(upToCount: 64 * 1024), !chunk.isEmpty {
            try Task.checkCancellation()
            try validator.consume(chunk)
        }
        try validator.finish()
    }
}

struct DictionaryIndexValidator {
    enum Mode { case definitions(length: UInt64), synonyms(words: UInt32) }
    private let expectedWords: UInt32
    private let expectedBytes: UInt64
    private let mode: Mode
    private var consumed: UInt64 = 0
    private var records: UInt64 = 0
    private var word = Data()
    private var suffix = Data()
    private var previous = Data()
    private var readingSuffix = false
    init(words: UInt32, bytes: UInt64, mode: Mode) {
        expectedWords = words; expectedBytes = bytes; self.mode = mode
        word.reserveCapacity(255); suffix.reserveCapacity(8); previous.reserveCapacity(255)
    }
    mutating func consume(_ data: Data) throws {
        guard consumed <= expectedBytes, UInt64(data.count) <= expectedBytes - consumed else { throw ImportError.integrity }
        consumed += UInt64(data.count)
        for byte in data {
            if readingSuffix {
                suffix.append(byte)
                let width: Int
                switch mode { case .definitions: width = 8; case .synonyms: width = 4 }
                if suffix.count == width {
                    let offset = integer(0)
                    switch mode {
                    case .definitions(let length):
                        let size = integer(4)
                        guard size > 0, offset <= length, size <= length - offset else { throw ImportError.integrity }
                    case .synonyms(let words):
                        guard offset < UInt64(words) else { throw ImportError.integrity }
                    }
                    records += 1
                    guard records <= UInt64(expectedWords) else { throw ImportError.integrity }
                    word.removeAll(keepingCapacity: true); suffix.removeAll(keepingCapacity: true)
                    readingSuffix = false
                }
            } else if byte == 0 {
                guard !word.isEmpty, String(data: word, encoding: .utf8) != nil else { throw ImportError.integrity }
                let folded = Data(word.map { (65 ... 90).contains($0) ? $0 + 32 : $0 })
                guard !folded.lexicographicallyPrecedes(previous) else { throw ImportError.integrity }
                previous = folded
                readingSuffix = true
            } else {
                guard word.count < 255 else { throw ImportError.resourceLimit }
                word.append(byte)
            }
        }
    }
    func finish() throws {
        guard consumed == expectedBytes, records == UInt64(expectedWords), !readingSuffix, word.isEmpty else {
            throw ImportError.integrity
        }
    }
    private func integer(_ offset: Int) -> UInt64 {
        (0 ..< 4).reduce(0) { ($0 << 8) | UInt64(suffix[offset + $1]) }
    }
}

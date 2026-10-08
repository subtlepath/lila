import Foundation

public enum CourseStoryKind: UInt8, Equatable, Sendable { case dialogue = 0, story = 1 }
public struct CourseStoryContext: Equatable, Identifiable, Sendable {
    public let id: UInt32
    public let title: String
    public let titleIsTruncated: Bool
    public let kind: CourseStoryKind
    public let lessonIdentity: UInt32?
    public var unitNumber: UInt16? { lessonIdentity.map { UInt16($0 >> 16) } }
    public var lessonNumber: UInt16? { lessonIdentity.map { UInt16($0 & 0xffff) } }
}

public struct CoursePackMetadata: Equatable, Sendable {
    public let major: UInt16
    public let minor: UInt16
    public let contentVersion: UInt32
    public let locale: String
    public let itemIdentities: [UInt32]
    public let itemHistoryCount: UInt32?
    public let recognitionItems: [UInt32]
    public let storyIdentities: [UInt32]
    public let legacyStoryIdentities: [UInt32]
    public let lessonIdentities: [UInt32]
    public let lessonCount: UInt16
    public let stories: [CourseStoryContext]
    init(major: UInt16, minor: UInt16, contentVersion: UInt32, locale: String,
         itemIdentities: [UInt32], recognitionItems: [UInt32], storyIdentities: [UInt32],
         lessonIdentities: [UInt32], lessonCount: UInt16, legacyStoryIdentities: [UInt32], stories: [CourseStoryContext] = [], itemHistoryCount: UInt32? = nil) {
        self.major = major; self.minor = minor; self.contentVersion = contentVersion; self.locale = locale
        self.itemIdentities = itemIdentities; self.recognitionItems = recognitionItems
        self.storyIdentities = storyIdentities; self.legacyStoryIdentities = legacyStoryIdentities
        self.lessonIdentities = lessonIdentities; self.lessonCount = lessonCount
        self.stories = stories
        self.itemHistoryCount = itemHistoryCount
    }
    public func containsHistoricalItem(_ uid: UInt32) -> Bool {
        guard uid > 0, uid != UInt32.max else { return false }
        if let itemHistoryCount { return uid <= itemHistoryCount }
        return itemIdentities.contains(uid)
    }
}

public struct CoursePackDetails: Equatable, Sendable {
    public let major: UInt16
    public let minor: UInt16
    public let contentVersion: UInt32
    public let locale: String
    init(_ metadata: CoursePackMetadata) throws {
        try self.init(major: metadata.major, minor: metadata.minor,
                      contentVersion: metadata.contentVersion, locale: metadata.locale)
    }
    init(major: UInt16, minor: UInt16, contentVersion: UInt32, locale: String) throws {
        guard major == 1, Self.validLocale(locale) else { throw StoreError.invalidValue }
        self.major = major; self.minor = minor
        self.contentVersion = contentVersion; self.locale = locale
    }
    static func validLocale(_ locale: String) -> Bool {
        guard (1 ... 8).contains(locale.utf8.count) else { return false }
        return locale.split(separator: "-", omittingEmptySubsequences: false).enumerated().allSatisfy { index, part in
            !part.isEmpty && part.utf8.allSatisfy {
                (65 ... 90).contains($0) || (97 ... 122).contains($0) || (index > 0 && (48 ... 57).contains($0))
            }
        }
    }
}

public enum CoursePackInspector {
    public enum ItemHistoryCompatibility: Equatable, Sendable {
        case compatible, identityBaseline, differentLanguage, missingHistory, removedHistory, reassignedIdentity
    }
    private struct Section {
        let offset: UInt64
        let size: UInt64
        let count: UInt64
        var stride: UInt64 { count == 0 ? 0 : size / count }
    }
    private static let sizes: [String: UInt64] = [
        "LEMM": 48, "LKEY": 8, "EKEY": 8, "FORM": 12, "VERB": 176,
        "SENT": 24, "TOKS": 8, "ITEM": 20, "IUID": 8, "DIST": 4,
        "UNIT": 20, "LESS": 32, "NOTE": 16, "NSPN": 8, "STOR": 24,
        "SLIN": 8, "SQST": 24, "PHRS": 12, "PENT": 8, "CONF": 24, "LEXS": 2, "STRS": 0
    ]
    private static let stringFields: [String: Int] = [
        "LEMM": 7, "LKEY": 1, "EKEY": 1, "NOTE": 1, "NSPN": 1,
        "SLIN": 1, "PENT": 1, "CONF": 1, "FORM": 2, "LESS": 2,
        "STOR": 2, "PHRS": 2, "SENT": 3, "UNIT": 3, "SQST": 5, "VERB": 43
    ]
    private static let crcTable: [UInt32] = (0 ..< 256).map { index in
        var value = UInt32(index)
        for _ in 0 ..< 8 { value = (value >> 1) ^ (value & 1 == 0 ? 0 : 0xedb88320) }
        return value
    }
    public static func inspect(_ url: URL) throws -> CoursePackMetadata {
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        return try inspect(file)
    }
    // Callers must keep both course files immutable for the duration of comparison.
    public static func compareItemHistory(current: URL, candidate: URL) throws -> ItemHistoryCompatibility {
        let previous = try FileHandle(forReadingFrom: current)
        defer { try? previous.close() }
        let next = try FileHandle(forReadingFrom: candidate)
        defer { try? next.close() }
        let currentMetadata = try inspect(previous)
        let candidateMetadata = try inspect(next)
        guard currentMetadata.locale.lowercased() == candidateMetadata.locale.lowercased() else {
            return .differentLanguage
        }
        func history(_ file: FileHandle) throws -> Section? {
            let header = try read(file, offset: 0, count: 48)
            let directory = integer(header, 32, 4), count = integer(header, 36, 2)
            for index in 0..<count {
                try Task.checkCancellation()
                let entry = try read(file, offset: directory + index * 16, count: 16)
                if entry.prefix(4) == Data("IDEN".utf8) {
                    return Section(offset: integer(entry, 4, 4), size: integer(entry, 8, 4),
                                   count: integer(entry, 12, 4))
                }
            }
            return nil
        }
        let old = try history(previous), new = try history(next)
        if old == nil, new != nil {
            return try sameLegacySections(previous, next) ? .identityBaseline : .missingHistory
        }
        guard let old, let new else { return .missingHistory }
        guard new.count >= old.count else { return .removedHistory }
        for index in 0..<old.count {
            try Task.checkCancellation()
            let before = try read(previous, offset: old.offset + index * 36, count: 36)
            let after = try read(next, offset: new.offset + index * 36, count: 36)
            if before != after { return .reassignedIdentity }
        }
        return .compatible
    }
    private static func sameLegacySections(_ previous: FileHandle, _ next: FileHandle) throws -> Bool {
        let oldHeader = try read(previous, offset: 0, count: 48)
        let newHeader = try read(next, offset: 0, count: 48)
        let oldDirectory = integer(oldHeader, 32, 4), newDirectory = integer(newHeader, 32, 4)
        let oldCount = integer(oldHeader, 36, 2), newCount = integer(newHeader, 36, 2)
        var nextIndex: UInt64 = 0
        var hasIdentity = false
        for index in 0..<oldCount {
            try Task.checkCancellation()
            let before = try read(previous, offset: oldDirectory + index * 16, count: 16)
            var after: Data
            repeat {
                guard nextIndex < newCount else { return false }
                after = try read(next, offset: newDirectory + nextIndex * 16, count: 16)
                nextIndex += 1
                if after.prefix(4) != Data("IDEN".utf8) { break }
                guard !hasIdentity else { return false }
                hasIdentity = true
            } while true
            guard before.prefix(4) == after.prefix(4), before[8..<16] == after[8..<16] else { return false }
            let size = integer(before, 8, 4)
            var consumed: UInt64 = 0
            while consumed < size {
                try Task.checkCancellation()
                let count = Int(min(UInt64(64 * 1024), size - consumed))
                let oldBytes = try read(previous, offset: integer(before, 4, 4) + consumed, count: count)
                let newBytes = try read(next, offset: integer(after, 4, 4) + consumed, count: count)
                guard oldBytes == newBytes else { return false }
                consumed += UInt64(count)
            }
        }
        while nextIndex < newCount {
            let entry = try read(next, offset: newDirectory + nextIndex * 16, count: 16)
            guard !hasIdentity, entry.prefix(4) == Data("IDEN".utf8) else { return false }
            hasIdentity = true
            nextIndex += 1
        }
        return hasIdentity
    }
    private static func inspect(_ file: FileHandle) throws -> CoursePackMetadata {
        let length = try file.seekToEnd()
        guard length >= 48, length <= UInt64(UInt32.max) else { throw ImportError.integrity }
        let header = try read(file, offset: 0, count: 48)
        guard header.prefix(4) == Data("TNTA".utf8) else { throw ImportError.integrity }
        guard integer(header, 4, 2) == 1 else { throw ImportError.unsupportedEntry }
        let directory = integer(header, 32, 4)
        let count = integer(header, 36, 2)
        let headerSize = integer(header, 38, 2)
        guard integer(header, 16, 4) == length, headerSize >= 48,
              directory >= headerSize, directory % 4 == 0,
              directory + count * 16 <= length else { throw ImportError.integrity }
        let entries = try read(file, offset: directory, count: Int(count * 16))
        var seen = Set<String>()
        seen.reserveCapacity(sizes.count)
        var sections: [String: Section] = [:]
        sections.reserveCapacity(sizes.count)
        for index in 0 ..< Int(count) {
            try Task.checkCancellation()
            let at = index * 16
            let tag = String(decoding: entries[at ..< at + 4], as: UTF8.self)
            let offset = integer(entries, at + 4, 4)
            let size = integer(entries, at + 8, 4)
            let records = integer(entries, at + 12, 4)
            guard offset % 4 == 0, offset + size <= length else { throw ImportError.integrity }
            if tag == "IDEN" {
                guard sections[tag] == nil, records < UInt64(UInt32.max), size == records * 36 else {
                    throw ImportError.integrity
                }
                sections[tag] = Section(offset: offset, size: size, count: records)
                continue
            }
            guard let minimum = sizes[tag] else { continue }
            guard seen.insert(tag).inserted else { throw ImportError.integrity }
            sections[tag] = Section(offset: offset, size: size, count: records)
            if minimum > 0 {
                guard (records == 0 && size == 0) || (records > 0 && size % records == 0 && size / records >= minimum) else {
                    throw ImportError.integrity
                }
            } else {
                guard size > 0, try read(file, offset: offset, count: 1)[0] == 0,
                      try read(file, offset: offset + size - 1, count: 1)[0] == 0 else { throw ImportError.integrity }
            }
        }
        guard seen.count == sizes.count else { throw ImportError.missingEntry }
        try validateStrings(file, sections: sections)
        let itemIdentity = try validateIdentities(file, sections: sections)
        try validateItemHistory(file, section: sections["IDEN"], items: itemIdentity.items)
        try validateContentRanges(file, sections: sections)
        try file.seek(toOffset: 0)
        var crc: UInt32 = 0xffffffff
        var position: UInt64 = 0
        while let data = try file.read(upToCount: 64 * 1024), !data.isEmpty {
            try Task.checkCancellation()
            for byte in data {
                let value: UInt8 = (20 ..< 24).contains(position) ? 0 : byte
                crc = crcTable[Int((crc ^ UInt32(value)) & 255)] ^ (crc >> 8)
                position += 1
            }
        }
        guard position == length, ~crc == UInt32(integer(header, 20, 4)) else { throw ImportError.integrity }
        let localeData = header[24 ..< 32].prefix(while: { $0 != 0 })
        guard let locale = String(data: Data(localeData), encoding: .utf8), CoursePackDetails.validLocale(locale),
              header[(24 + localeData.count) ..< 32].allSatisfy({ $0 == 0 }) else { throw ImportError.integrity }
        let lessons = try lessonIdentities(file, sections: sections)
        let stories = try storyIdentities(file, sections: sections, lessons: lessons)
        return CoursePackMetadata(major: 1, minor: UInt16(integer(header, 6, 2)),
                                  contentVersion: UInt32(integer(header, 8, 4)), locale: locale,
                                  itemIdentities: itemIdentity.items, recognitionItems: itemIdentity.recognition,
                                  storyIdentities: stories.distributed, lessonIdentities: lessons,
                                  lessonCount: UInt16(sections["LESS"]!.count), legacyStoryIdentities: stories.legacy,
                                  stories: stories.contexts,
                                  itemHistoryCount: sections["IDEN"].map { UInt32($0.count) })
    }
    private static func validateItemHistory(_ file: FileHandle, section: Section?, items: [UInt32]) throws {
        guard let section else { return }
        for index in 0..<section.count {
            try Task.checkCancellation()
            let record = try read(file, offset: section.offset + index * 36, count: 36)
            guard integer(record, 0, 4) == index + 1, record[4..<36].contains(where: { $0 != 0 }) else {
                throw ImportError.integrity
            }
        }
        guard items.allSatisfy({ $0 > 0 && UInt64($0) <= section.count }) else { throw ImportError.integrity }
    }
    private static func validateStrings(_ file: FileHandle, sections: [String: Section]) throws {
        guard let strings = sections["STRS"] else { throw ImportError.missingEntry }
        for (tag, fields) in stringFields {
            guard let section = sections[tag] else { throw ImportError.missingEntry }
            for index in 0..<section.count {
                try Task.checkCancellation()
                let record = try read(file, offset: section.offset + index * section.stride, count: fields * 4)
                for field in 0..<fields {
                    guard integer(record, field * 4, 4) < strings.size else { throw ImportError.integrity }
                }
            }
        }
        var consumed: UInt64 = 0
        var remaining = 0
        var scalar: UInt32 = 0, minimum: UInt32 = 0
        while consumed < strings.size {
            try Task.checkCancellation()
            let bytes = try read(file, offset: strings.offset + consumed,
                                 count: Int(min(64 * 1024, strings.size - consumed)))
            for byte in bytes {
                if remaining > 0 {
                    guard byte & 0xc0 == 0x80 else { throw ImportError.integrity }
                    scalar = (scalar << 6) | UInt32(byte & 0x3f)
                    remaining -= 1
                    if remaining == 0 {
                        guard scalar >= minimum, scalar <= 0x10ffff,
                              !(0xd800...0xdfff).contains(scalar) else { throw ImportError.integrity }
                    }
                } else {
                    switch byte {
                    case 0...0x7f: break
                    case 0xc2...0xdf: scalar = UInt32(byte & 0x1f); minimum = 0x80; remaining = 1
                    case 0xe0...0xef: scalar = UInt32(byte & 0x0f); minimum = 0x800; remaining = 2
                    case 0xf0...0xf4: scalar = UInt32(byte & 7); minimum = 0x10000; remaining = 3
                    default: throw ImportError.integrity
                    }
                }
            }
            consumed += UInt64(bytes.count)
        }
        guard remaining == 0 else { throw ImportError.integrity }
    }
    private static func storyIdentities(_ file: FileHandle, sections: [String: Section], lessons: [UInt32]) throws -> (distributed: [UInt32], legacy: [UInt32], contexts: [CourseStoryContext]) {
        guard let stories = sections["STOR"], let strings = sections["STRS"], stories.count <= 65535 else {
            throw ImportError.integrity
        }
        var identities: [UInt32] = []; identities.reserveCapacity(Int(stories.count))
        var legacy: [UInt32] = []; legacy.reserveCapacity(Int(stories.count))
        var contexts: [CourseStoryContext] = []; contexts.reserveCapacity(Int(stories.count))
        var seen = Set<UInt32>(); seen.reserveCapacity(Int(stories.count))
        for index in 0..<stories.count {
            try Task.checkCancellation()
            let story = try read(file, offset: stories.offset + index * stories.stride, count: 24)
            var position = integer(story, 0, 4)
            guard position > 0, position < strings.size, story[20] <= 1 else { throw ImportError.integrity }
            let lesson = integer(story, 14, 2)
            guard lesson == 65535 || lesson < UInt64(lessons.count) else { throw ImportError.integrity }
            let lessonIdentity = lesson == 65535 ? UInt32.max : lessons[Int(lesson)]
            var hash: UInt32 = 2166136261, legacyHash: UInt32 = 2166136261, terminated = false
            var displayBytes = Data(); displayBytes.reserveCapacity(256)
            var truncated = false
            for byte in "TST1".utf8 { hash = (hash ^ UInt32(byte)) &* 16777619 }
            hash = (hash ^ UInt32(story[20])) &* 16777619
            for index in 0..<4 {
                hash = (hash ^ UInt32(UInt8(truncatingIfNeeded: lessonIdentity >> (8 * index)))) &* 16777619
            }
            while position < strings.size, !terminated {
                try Task.checkCancellation()
                let bytes = try read(file, offset: strings.offset + position, count: Int(min(256, strings.size - position)))
                for byte in bytes {
                    if byte == 0 { terminated = true; break }
                    hash = (hash ^ UInt32(byte)) &* 16777619
                    legacyHash = (legacyHash ^ UInt32(byte)) &* 16777619
                    if displayBytes.count < 256 { displayBytes.append(byte) }
                    else { truncated = true }
                }
                position += UInt64(bytes.count)
            }
            guard terminated else { throw ImportError.integrity }
            let identity = hash == 0 ? 1 : hash
            guard identity != UInt32.max, seen.insert(identity).inserted else { throw ImportError.integrity }
            identities.append(identity)
            legacy.append(legacyHash == 0 ? 1 : legacyHash)
            while String(data: displayBytes, encoding: .utf8) == nil, !displayBytes.isEmpty { displayBytes.removeLast() }
            guard let title = String(data: displayBytes, encoding: .utf8), let kind = CourseStoryKind(rawValue: story[20]) else {
                throw ImportError.integrity
            }
            contexts.append(CourseStoryContext(id: identity, title: title + (truncated ? "…" : ""),
                titleIsTruncated: truncated, kind: kind, lessonIdentity: lesson == 65535 ? nil : lessonIdentity))
        }
        return (identities, legacy, contexts)
    }
    private static func lessonIdentities(_ file: FileHandle, sections: [String: Section]) throws -> [UInt32] {
        guard let units = sections["UNIT"], let lessons = sections["LESS"],
              units.count <= 65535, lessons.count <= 65535 else {
            throw ImportError.integrity
        }
        var numbers: [UInt16] = []; numbers.reserveCapacity(Int(units.count))
        var unitNumbers = Set<UInt16>(); unitNumbers.reserveCapacity(Int(units.count))
        var owners = [UInt16](repeating: UInt16.max, count: Int(lessons.count))
        for index in 0..<units.count {
            try Task.checkCancellation()
            let unit = try read(file, offset: units.offset + index * units.stride, count: 20)
            let number = UInt16(integer(unit, 12, 2))
            let first = integer(unit, 14, 2), count = integer(unit, 16, 2)
            guard unitNumbers.insert(number).inserted,
                  first <= lessons.count, count <= lessons.count - first else { throw ImportError.integrity }
            for lesson in first..<first + count {
                guard owners[Int(lesson)] == UInt16.max else { throw ImportError.integrity }
                owners[Int(lesson)] = UInt16(index)
            }
            numbers.append(number)
        }
        var identities: [UInt32] = []; identities.reserveCapacity(Int(lessons.count))
        var seen = Set<UInt32>(); seen.reserveCapacity(Int(lessons.count))
        for index in 0..<lessons.count {
            try Task.checkCancellation()
            let lesson = try read(file, offset: lessons.offset + index * lessons.stride, count: 32)
            let unit = integer(lesson, 14, 2), number = UInt32(integer(lesson, 16, 2))
            guard unit < units.count, number > 0, owners[Int(index)] == UInt16(unit) else {
                throw ImportError.integrity
            }
            let identity = (UInt32(numbers[Int(unit)]) << 16) | number
            guard identity != UInt32.max, seen.insert(identity).inserted else { throw ImportError.integrity }
            identities.append(identity)
        }
        return identities
    }
    private static func validateIdentities(_ file: FileHandle, sections: [String: Section]) throws -> (items: [UInt32], recognition: [UInt32]) {
        guard let items = sections["ITEM"], let index = sections["IUID"], let lessons = sections["LESS"],
              let candidates = sections["DIST"], items.count <= 65_535, lessons.count <= 65_535, index.count == items.count else {
            throw ImportError.integrity
        }
        var recognition: [UInt32] = []; recognition.reserveCapacity(Int(items.count))
        var identities: [UInt32] = []
        identities.reserveCapacity(Int(items.count))
        var seen = Set<UInt32>()
        seen.reserveCapacity(Int(items.count))
        for number in 0 ..< items.count {
            try Task.checkCancellation()
            let record = try read(file, offset: items.offset + number * items.stride, count: 20)
            let uid = UInt32(integer(record, 0, 4))
            let lesson = integer(record, 6, 2)
            let prerequisite = integer(record, 12, 2)
            let first = integer(record, 16, 4)
            guard uid != 0, uid != UInt32.max, seen.insert(uid).inserted,
                  record[4] <= 6, lesson == 65_535 || lesson < lessons.count,
                  prerequisite == 65_535 || (prerequisite < items.count && prerequisite != number),
                  first <= candidates.count, UInt64(record[5]) <= candidates.count - first else {
                throw ImportError.integrity
            }
            try validateOperands(file, sections: sections, item: record)
            identities.append(uid)
            if record[4] == 0 { recognition.append(uid) }
        }
        var previous: UInt32 = 0
        for number in 0 ..< index.count {
            try Task.checkCancellation()
            let record = try read(file, offset: index.offset + number * index.stride, count: 8)
            let uid = UInt32(integer(record, 0, 4))
            let item = integer(record, 4, 2)
            guard uid > previous, item < items.count, identities[Int(item)] == uid else { throw ImportError.integrity }
            previous = uid
        }
        return (identities, recognition)
    }
    private static func record(_ file: FileHandle, sections: [String: Section], tag: String, index: UInt64) throws -> Data {
        guard let section = sections[tag], let size = sizes[tag], size > 0, index < section.count else {
            throw ImportError.integrity
        }
        return try read(file, offset: section.offset + index * section.stride, count: Int(size))
    }
    private static func validateOperands(_ file: FileHandle, sections: [String: Section], item: Data) throws {
        let kind = item[4], count = UInt64(item[5])
        let a = integer(item, 8, 2), b = integer(item, 10, 2)
        switch kind {
        case 0, 1, 4:
            guard a < sections["LEMM"]!.count, b == 0 else { throw ImportError.integrity }
        case 2:
            let sentence = try record(file, sections: sections, tag: "SENT", index: a)
            let first = integer(sentence, 12, 4), tokens = UInt64(sentence[18])
            guard b < tokens, first <= sections["TOKS"]!.count,
                  tokens <= sections["TOKS"]!.count - first else { throw ImportError.integrity }
        case 3:
            let lemma = try record(file, sections: sections, tag: "LEMM", index: a)
            guard lemma[40] == 2, integer(lemma, 38, 2) < sections["VERB"]!.count else {
                throw ImportError.integrity
            }
            if b & 0xf000 == 0x1000 {
                guard (b >> 4) & 15 < 8, b & 15 < 5 else { throw ImportError.integrity }
            } else {
                guard (0x2000...0x2002).contains(b) else { throw ImportError.integrity }
            }
        case 5:
            let phrase = try record(file, sections: sections, tag: "PENT", index: b)
            guard a < sections["SENT"]!.count, integer(phrase, 4, 2) == a else { throw ImportError.integrity }
        case 6:
            guard a < sections["SENT"]!.count, b == 0 else { throw ImportError.integrity }
        default: throw ImportError.integrity
        }
        guard (kind != 4 && kind != 6) || count == 0 else { throw ImportError.integrity }
        let first = integer(item, 16, 4)
        for index in 0..<count {
            let candidate = integer(try record(file, sections: sections, tag: "DIST", index: first + index), 0, 4)
            switch kind {
            case 0, 1:
                guard candidate < sections["LEMM"]!.count else { throw ImportError.integrity }
            case 5:
                guard candidate < sections["SENT"]!.count else { throw ImportError.integrity }
            case 2, 3:
                guard candidate < sections["STRS"]!.size else { throw ImportError.integrity }
            default: break
            }
        }
    }
    private static func validateContentRanges(_ file: FileHandle, sections: [String: Section]) throws {
        func range(_ first: UInt64, _ count: UInt64, _ tag: String) -> Bool {
            let total = sections[tag]!.count
            return first <= total && count <= total - first
        }
        let lessons = sections["LESS"]!.count, stories = sections["STOR"]!.count
        guard lessons <= 65535, stories <= 65535 else { throw ImportError.integrity }
        for index in 0..<lessons {
            try Task.checkCancellation()
            let lesson = try record(file, sections: sections, tag: "LESS", index: index)
            let dialogue = integer(lesson, 22, 2)
            guard range(integer(lesson, 8, 4), integer(lesson, 12, 2), "ITEM"),
                  range(integer(lesson, 18, 2), integer(lesson, 20, 2), "NOTE"),
                  range(integer(lesson, 24, 2), integer(lesson, 26, 2), "SENT"),
                  dialogue == 65535 || dialogue < stories else { throw ImportError.integrity }
        }
        for index in 0..<stories {
            try Task.checkCancellation()
            let story = try record(file, sections: sections, tag: "STOR", index: index)
            let lesson = integer(story, 14, 2)
            guard lesson == 65535 || lesson < lessons,
                  range(integer(story, 8, 4), integer(story, 12, 2), "SLIN"),
                  range(integer(story, 16, 2), UInt64(story[18]), "SQST") else { throw ImportError.integrity }
        }
    }
    private static func read(_ file: FileHandle, offset: UInt64, count: Int) throws -> Data {
        try file.seek(toOffset: offset)
        let data = try file.read(upToCount: count) ?? Data()
        guard data.count == count else { throw ImportError.integrity }
        return data
    }
    private static func integer(_ data: Data, _ offset: Int, _ width: Int) -> UInt64 {
        (0 ..< width).reduce(0) { $0 | (UInt64(data[offset + $1]) << ($1 * 8)) }
    }
}

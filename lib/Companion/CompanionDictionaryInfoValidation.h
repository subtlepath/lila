#pragma once

#include <cstring>

#include "CompanionInventoryIndex.h"

namespace companion {
struct DictionaryInfoDetails {
  uint32_t words = 0, indexBytes = 0, synonyms = 0;
  uint16_t version = 0;  // 242 or 300.
  bool hasSynonyms = false, htmlDefinitions = false;
  bool operator==(const DictionaryInfoDetails&) const = default;
};
// UTF-8 metadata with byte-exact field names. Session-owned; scratch and source are borrowed.
class DictionaryInfoValidation final {
 public:
  using Progress = bool (*)(void*);
  DictionaryInfoValidation(InventoryIndexStorage& source, std::span<uint8_t> scratch, Progress progress = nullptr,
                           void* context = nullptr)
      : source(source), scratch(scratch), progress(progress), context(context) {}
  bool validate(DictionaryInfoDetails& output) {
    uint64_t size = 0;
    cacheStart = UINT32_MAX;
    cached = steps = 0;
    failed = false;
    if (scratch.empty() || !source.size(size) || size == 0 || size > 65536) return false;
    length = static_cast<uint32_t>(size);
    for (uint32_t at = 0; at < length;) {
      uint32_t scalar;
      if (!nextScalar(at, length, scalar) || scalar == 0) return false;
    }
    Line header;
    if (!line(0, header) || !signature(header)) return false;
    DictionaryInfoDetails parsed;
    uint8_t seen = 0;
    for (uint32_t at = header.next; at < length;) {
      Line field;
      if (!line(at, field)) return false;
      at = field.next;
      if (field.end == field.start) continue;
      if (field.equal == UINT32_MAX || field.equal == field.start || field.equal >= field.end) return false;
      if (!unique(field, header.next)) return false;
      const auto value = field.equal + 1;
      if (matches(field.start, field.equal, "version")) {
        if (matches(value, field.end, "2.4.2"))
          parsed.version = 242;
        else if (matches(value, field.end, "3.0.0"))
          parsed.version = 300;
        else
          return false;
        seen |= 1;
      } else if (matches(field.start, field.equal, "bookname")) {
        if (!name(value, field.end)) return false;
        seen |= 2;
      } else if (matches(field.start, field.equal, "wordcount")) {
        if (!decimal(value, field.end, parsed.words)) return false;
        seen |= 4;
      } else if (matches(field.start, field.equal, "idxfilesize")) {
        if (!decimal(value, field.end, parsed.indexBytes)) return false;
        seen |= 8;
      } else if (matches(field.start, field.equal, "synwordcount")) {
        if (!decimal(value, field.end, parsed.synonyms)) return false;
        parsed.hasSynonyms = true;
      } else if (matches(field.start, field.equal, "idxoffsetbits")) {
        if (field.end >= 2047 || !matches(value, field.end, "32")) return false;
        seen |= 16;
      } else if (matches(field.start, field.equal, "sametypesequence")) {
        if (field.end >= 2047 || value == field.end) return false;
        for (uint32_t position = value; position < field.end; ++position) {
          uint8_t letter;
          if (!byte(position, letter) || !((letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z')))
            return false;
        }
        parsed.htmlDefinitions = matches(value, field.end, "h");
        seen |= 32;
      }
    }
    if ((seen & 15) != 15 || !prefixOccurrences("idxoffsetbits", seen & 16 ? 1 : 0) ||
        !prefixOccurrences("sametypesequence", seen & 32 ? 1 : 0) || !source.size(size) || size != length)
      return false;
    output = parsed;
    return true;
  }

 private:
  struct Line {
    uint32_t start = 0, equal = UINT32_MAX, end = 0, rawEnd = 0, next = 0;
  };
  InventoryIndexStorage& source;
  std::span<uint8_t> scratch;
  Progress progress;
  void* context;
  uint32_t length = 0, cacheStart = UINT32_MAX, cached = 0;
  uint16_t steps = 0;
  bool failed = true;
  bool byte(uint32_t at, uint8_t& value) {
    if (failed || at >= length) {
      failed = true;
      return false;
    }
    if (++steps == 512) {
      steps = 0;
      if (progress && !progress(context)) {
        failed = true;
        return false;
      }
    }
    if (cacheStart == UINT32_MAX || at < cacheStart || at - cacheStart >= cached) {
      const auto width = static_cast<uint32_t>(std::min<size_t>(scratch.size(), length));
      cacheStart = at / width * width;
      cached = std::min(width, length - cacheStart);
      if (!source.read(cacheStart, scratch.first(cached))) {
        cacheStart = UINT32_MAX;
        failed = true;
        return false;
      }
    }
    value = scratch[at - cacheStart];
    return true;
  }
  bool line(uint32_t start, Line& result) {
    result = {};
    result.start = start;
    uint32_t at = start;
    uint8_t value = 0;
    while (at < length) {
      if (!byte(at, value)) return false;
      if (value == '\n') break;
      if (value == '=' && result.equal == UINT32_MAX) result.equal = at;
      ++at;
    }
    result.rawEnd = result.end = at;
    result.next = at < length ? at + 1 : length;
    if (at > start) {
      if (!byte(at - 1, value)) return false;
      if (value == '\r') --result.end;
    }
    return true;
  }
  bool matches(uint32_t start, uint32_t end, const char* text) {
    const auto count = std::strlen(text);
    if (end - start != count) return false;
    for (uint32_t index = 0; index < count; ++index) {
      uint8_t value;
      if (!byte(start + index, value) || value != static_cast<uint8_t>(text[index])) return false;
    }
    return true;
  }
  bool unique(const Line& current, uint32_t start) {
    for (uint32_t at = start; at < current.start;) {
      Line previous;
      if (!line(at, previous)) return false;
      at = previous.next;
      if (previous.end == previous.start || previous.equal == UINT32_MAX ||
          previous.equal - previous.start != current.equal - current.start)
        continue;
      bool same = true;
      for (uint32_t index = 0; index < current.equal - current.start; ++index) {
        uint8_t left, right;
        if (!byte(previous.start + index, left) || !byte(current.start + index, right)) return false;
        if (left != right) {
          same = false;
          break;
        }
      }
      if (same) return false;
    }
    return true;
  }
  bool decimal(uint32_t at, uint32_t end, uint32_t& output) {
    if (at == end) return false;
    uint32_t number = 0;
    while (at < end) {
      uint8_t digit;
      if (!byte(at++, digit) || digit < '0' || digit > '9' || number > (UINT32_MAX - (digit - '0')) / 10) return false;
      number = number * 10 + digit - '0';
    }
    output = number;
    return true;
  }
  bool nextScalar(uint32_t& at, uint32_t end, uint32_t& scalar) {
    uint8_t lead;
    if (at >= end || !byte(at++, lead)) return false;
    if (lead < 128) {
      scalar = lead;
      return true;
    }
    unsigned count;
    uint32_t minimum;
    if (lead >= 0xc2 && lead <= 0xdf) {
      count = 1;
      minimum = 0x80;
      scalar = lead & 0x1f;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      count = 2;
      minimum = 0x800;
      scalar = lead & 0xf;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      count = 3;
      minimum = 0x10000;
      scalar = lead & 7;
    } else
      return false;
    while (count--) {
      uint8_t continuation;
      if (at >= end || !byte(at++, continuation) || (continuation & 0xc0) != 0x80) return false;
      scalar = (scalar << 6) | (continuation & 0x3f);
    }
    return scalar >= minimum && scalar <= 0x10ffff && (scalar < 0xd800 || scalar > 0xdfff);
  }
  static bool newline(uint32_t scalar) {
    return (scalar >= 10 && scalar <= 13) || scalar == 0x85 || scalar == 0x2028 || scalar == 0x2029;
  }
  bool signature(const Line& header) {
    static constexpr char SIGNATURE[] = "StarDict's dict ifo file";
    uint32_t at = header.start, scalar = 0;
    do {
      if (!nextScalar(at, header.rawEnd, scalar)) return false;
    } while (newline(scalar));
    if (scalar != SIGNATURE[0]) return false;
    for (unsigned index = 1; index < sizeof(SIGNATURE) - 1; ++index) {
      if (!nextScalar(at, header.rawEnd, scalar) || scalar != static_cast<uint8_t>(SIGNATURE[index])) return false;
    }
    while (at < header.rawEnd)
      if (!nextScalar(at, header.rawEnd, scalar) || !newline(scalar)) return false;
    return true;
  }
  bool name(uint32_t at, uint32_t end) {
    while (at < end) {
      uint32_t scalar;
      if (!nextScalar(at, end, scalar)) return false;
      const bool whitespace = scalar == 9 || scalar == 32 || scalar == 0xa0 || scalar == 0x1680 ||
                              (scalar >= 0x2000 && scalar <= 0x200b) || scalar == 0x202f || scalar == 0x205f ||
                              scalar == 0x3000;
      if (!whitespace) return true;
    }
    return false;
  }
  bool prefixOccurrences(const char* text, unsigned expected) {
    const auto size = std::strlen(text);
    const auto end = std::min<uint32_t>(length, 2047);
    unsigned found = 0;
    for (uint32_t at = 0; at + size <= end; ++at) {
      if (matches(at, at + size, text)) {
        ++found;
        at += size - 1;
      }
    }
    return !failed && found == expected;
  }
};
}  // namespace companion

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "CompanionInventoryIndex.h"

namespace companion {
enum class DictionaryIndexMode : uint8_t { Definitions = 1, Synonyms = 2 };
// Session-owned outside the task stack. No heap or per-record allocation.
class DictionaryIndexValidation final {
 public:
  static constexpr size_t MAX_WORD_BYTES = 255;
  using Progress = bool (*)(void*);
  bool validate(InventoryIndexStorage& source, std::span<uint8_t> scratch, uint32_t words, uint64_t bytes,
                DictionaryIndexMode selectedMode, uint64_t limit, Progress progress = nullptr,
                void* context = nullptr) {
    if (!begin(words, bytes, selectedMode, limit)) return false;
    uint64_t actual = 0;
    if (scratch.empty() || !source.size(actual) || actual != bytes) return failure();
    uint64_t offset = 0;
    while (offset < bytes) {
      const auto count = static_cast<size_t>(std::min<uint64_t>(scratch.size(), bytes - offset));
      auto chunk = scratch.first(count);
      if ((progress && !progress(context)) || !source.read(offset, chunk) || !consume(chunk)) return failure();
      offset += count;
    }
    if (!source.size(actual) || actual != bytes) return failure();
    return finish();
  }
  bool begin(uint32_t words, uint64_t bytes, DictionaryIndexMode selectedMode, uint64_t limit) {
    failed = true;
    complete = false;
    if ((selectedMode != DictionaryIndexMode::Definitions && selectedMode != DictionaryIndexMode::Synonyms) ||
        (selectedMode == DictionaryIndexMode::Synonyms && limit > UINT32_MAX))
      return false;
    expectedWords = words;
    expectedBytes = bytes;
    mode = selectedMode;
    bound = limit;
    consumed = records = 0;
    wordLength = previousLength = suffixLength = 0;
    readingSuffix = greater = false;
    remaining = 0;
    scalar = minimum = 0;
    failed = false;
    return true;
  }
  bool consume(std::span<const uint8_t> bytes) {
    if (failed || (complete && !bytes.empty()) || consumed > expectedBytes || bytes.size() > expectedBytes - consumed)
      return failure();
    consumed += bytes.size();
    for (const auto byte : bytes) {
      if (readingSuffix) {
        suffix[suffixLength++] = byte;
        const unsigned width = mode == DictionaryIndexMode::Definitions ? 8 : 4;
        if (suffixLength == width) {
          const auto offset = number(0);
          if (mode == DictionaryIndexMode::Definitions) {
            const auto size = number(4);
            if (size == 0 || offset > bound || size > bound - offset) return failure();
          } else if (offset >= bound)
            return failure();
          if (++records > expectedWords) return failure();
          readingSuffix = greater = false;
          suffixLength = wordLength = 0;
        }
      } else if (byte == 0) {
        if (wordLength == 0 || remaining != 0 || (!greater && wordLength < previousLength)) return failure();
        previousLength = wordLength;
        readingSuffix = true;
      } else {
        if (wordLength == MAX_WORD_BYTES || !utf8(byte)) return failure();
        const uint8_t folded = byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
        // Compare before overwriting the previous word's byte in place.
        if (!greater) {
          if (wordLength >= previousLength || folded > word[wordLength])
            greater = true;
          else if (folded < word[wordLength])
            return failure();
        }
        word[wordLength++] = folded;
      }
    }
    return true;
  }
  bool finish() {
    if (failed || consumed != expectedBytes || records != expectedWords || readingSuffix || wordLength || remaining)
      return failure();
    complete = true;
    return true;
  }

 private:
  std::array<uint8_t, MAX_WORD_BYTES> word{};
  std::array<uint8_t, 8> suffix{};
  uint64_t consumed = 0, records = 0, expectedBytes = 0, bound = 0;
  uint32_t expectedWords = 0, scalar = 0, minimum = 0;
  uint16_t wordLength = 0, previousLength = 0;
  uint8_t suffixLength = 0, remaining = 0;
  DictionaryIndexMode mode = DictionaryIndexMode::Definitions;
  bool failed = true, complete = false, readingSuffix = false, greater = false;
  bool failure() {
    failed = true;
    return false;
  }
  uint32_t number(unsigned at) const {
    uint32_t value = 0;
    for (unsigned byte = 0; byte < 4; ++byte) value = (value << 8) | suffix[at + byte];
    return value;
  }
  bool utf8(uint8_t byte) {
    if (remaining) {
      if ((byte & 0xc0) != 0x80) return false;
      scalar = (scalar << 6) | (byte & 0x3f);
      return --remaining != 0 || (scalar >= minimum && scalar <= 0x10ffff && (scalar < 0xd800 || scalar > 0xdfff));
    }
    if (byte <= 0x7f) return true;
    if (byte >= 0xc2 && byte <= 0xdf) {
      remaining = 1;
      scalar = byte & 0x1f;
      minimum = 0x80;
    } else if (byte >= 0xe0 && byte <= 0xef) {
      remaining = 2;
      scalar = byte & 0xf;
      minimum = 0x800;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
      remaining = 3;
      scalar = byte & 7;
      minimum = 0x10000;
    } else
      return false;
    return true;
  }
};
}  // namespace companion

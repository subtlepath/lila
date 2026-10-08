#pragma once

#include <algorithm>

#include "CompanionInventoryIndex.h"

namespace companion {
struct VectorFontDetails {
  uint16_t faces = 0;
  bool operator==(const VectorFontDetails&) const = default;
};
// Session-owned; scratch is borrowed. No face/table list is allocated.
class VectorFontValidation final {
 public:
  VectorFontValidation(InventoryIndexStorage& source, std::span<uint8_t> scratch) : source(source), scratch(scratch) {}
  bool validate(VectorFontDetails& output) {
    if (scratch.size() < 16 || !source.size(length) || !read(0, 12)) return false;
    if (number(0, 4) != 0x74746366) {
      if (!face(0)) return false;
      output = {1};
      return true;
    }
    const auto version = number(4, 4), count = number(8, 4);
    if ((version != 0x10000 && version != 0x20000) || count == 0 || count > 256 || 12 + uint64_t(count) * 4 > length)
      return false;
    for (uint32_t index = 0; index < count; ++index) {
      if (!read(12 + uint64_t(index) * 4, 4)) return false;
      const auto offset = number(0, 4);
      for (uint32_t previous = 0; previous < index; ++previous) {
        if (!read(12 + uint64_t(previous) * 4, 4) || number(0, 4) == offset) return false;
      }
      if (!face(offset)) return false;
    }
    output = {static_cast<uint16_t>(count)};
    return true;
  }

 private:
  InventoryIndexStorage& source;
  std::span<uint8_t> scratch;
  uint64_t length = 0;
  bool read(uint64_t offset, size_t count) {
    return offset <= length && count <= length - offset && source.read(offset, scratch.first(count));
  }
  uint32_t number(size_t at, size_t count) const {
    uint32_t result = 0;
    for (size_t byte = 0; byte < count; ++byte) result = (result << 8) | scratch[at + byte];
    return result;
  }
  bool checksum(uint32_t tag, uint32_t start, uint32_t size, uint32_t expected) {
    uint64_t consumed = 0;
    uint32_t sum = 0;
    const auto capacity = scratch.size() & ~size_t{3};
    while (consumed < size) {
      const auto count = static_cast<size_t>(std::min<uint64_t>(capacity, uint64_t(size) - consumed));
      if (!read(uint64_t(start) + consumed, count)) return false;
      for (size_t word = 0; word < count; word += 4) {
        uint32_t value = 0;
        for (size_t byte = 0; byte < 4; ++byte) {
          const auto position = word + byte;
          const bool adjustment = tag == 0x68656164 && consumed + position >= 8 && consumed + position < 12;
          value = (value << 8) | (position < count && !adjustment ? scratch[position] : 0);
        }
        sum += value;
      }
      consumed += count;
    }
    return sum == expected;
  }
  bool face(uint32_t offset) {
    if (offset % 4 || offset > length || length - offset < 12 || !read(offset, 12)) return false;
    const auto version = number(0, 4), count = number(4, 2);
    if ((version != 0x10000 && version != 0x4f54544f && version != 0x74727565) || count == 0 || count > 4096 ||
        uint64_t(count) * 16 > length - offset - 12)
      return false;
    uint32_t previous = 0;
    uint16_t present = 0;
    for (uint32_t index = 0; index < count; ++index) {
      if (!read(uint64_t(offset) + 12 + uint64_t(index) * 16, 16)) return false;
      for (unsigned byte = 0; byte < 4; ++byte)
        if (scratch[byte] < 32 || scratch[byte] > 126) return false;
      const auto tag = number(0, 4), expected = number(4, 4), start = number(8, 4), size = number(12, 4);
      if ((index && tag <= previous) || start % 4 || start > length || size > length - start) return false;
      previous = tag;
      struct Requirement {
        uint32_t tag, minimum;
        uint16_t bit;
      };
      static constexpr Requirement REQUIRED[] = {
          {0x68656164, 54, 1}, {0x68686561, 36, 2}, {0x6d617870, 6, 4},   {0x686d7478, 4, 8},   {0x636d6170, 4, 16},
          {0x6e616d65, 6, 32}, {0x676c7966, 0, 64}, {0x6c6f6361, 0, 128}, {0x43464620, 0, 256}, {0x43464632, 0, 512}};
      for (const auto& required : REQUIRED) {
        if (tag == required.tag) {
          if (size < required.minimum) return false;
          present |= required.bit;
        }
      }
      if (!checksum(tag, start, size, expected)) return false;
    }
    return (present & 63) == 63 && (version == 0x4f54544f ? (present & 768) != 0 : (present & 192) == 192);
  }
};
}  // namespace companion

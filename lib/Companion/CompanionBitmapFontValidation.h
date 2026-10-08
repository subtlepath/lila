#pragma once

#include <algorithm>
#include <cstring>

#include "CompanionInventoryIndex.h"

namespace companion {
struct BitmapFontDetails {
  uint16_t version = 0;
  uint8_t styles = 0;
  bool operator==(const BitmapFontDetails&) const = default;
};
// Session-owned validator. Scratch is borrowed and needs at least 32 bytes.
class BitmapFontValidation final {
 public:
  BitmapFontValidation(InventoryIndexStorage& source, std::span<uint8_t> scratch) : source(source), scratch(scratch) {}
  bool validate(BitmapFontDetails& output) {
    if (scratch.size() < 32 || !source.size(length) || !read(0, 32)) return false;
    static constexpr uint8_t MAGIC[] = {'C', 'P', 'F', 'O', 'N', 'T', 0, 0};
    if (std::memcmp(scratch.data(), MAGIC, sizeof(MAGIC)) != 0 || number(8, 2) != 4 || (number(10, 2) & ~uint32_t{1}))
      return false;
    bits = scratch[10] & 1 ? 2 : 1;
    const uint8_t count = scratch[12];
    if (count == 0 || count > 4) return false;
    tocEnd = 32 + uint64_t(count) * 32;
    uint8_t styles = 0;
    for (unsigned index = 0; index < count; ++index) {
      if (!read(32 + uint64_t(index) * 32, 32)) return false;
      const uint8_t id = scratch[0];
      if (id >= 4 || (styles & (1u << id))) return false;
      styles |= 1u << id;
      if (!loadStyle() || !validateIntervals() || !validateGlyphs() ||
          !validateKern(style.glyphStart + uint64_t(style.glyphs) * 16, style.left, style.leftClasses) ||
          !validateKern(style.glyphStart + uint64_t(style.glyphs) * 16 + uint64_t(style.left) * 3, style.right,
                        style.rightClasses) ||
          !validateLigatures())
        return false;
    }
    output = {4, styles};
    return true;
  }

 private:
  struct Style {
    uint32_t intervals = 0, glyphs = 0, start = 0;
    uint16_t left = 0, right = 0;
    uint8_t leftClasses = 0, rightClasses = 0, ligatures = 0;
    uint64_t glyphStart = 0, bitmapStart = 0;
  } style;
  InventoryIndexStorage& source;
  std::span<uint8_t> scratch;
  uint64_t length = 0, tocEnd = 0;
  uint8_t bits = 0;
  bool read(uint64_t offset, size_t count) {
    return offset <= length && count <= length - offset && source.read(offset, scratch.first(count));
  }
  uint32_t number(size_t at, size_t width) const {
    uint32_t result = 0;
    for (size_t byte = 0; byte < width; ++byte) result |= uint32_t(scratch[at + byte]) << (byte * 8);
    return result;
  }
  bool loadStyle() {
    style.intervals = number(4, 4);
    style.glyphs = number(8, 4);
    style.left = number(17, 2);
    style.right = number(19, 2);
    style.leftClasses = scratch[21];
    style.rightClasses = scratch[22];
    style.ligatures = scratch[23];
    style.start = number(24, 4);
    if (style.intervals > 4096 || style.glyphs > 65536 || style.left > 4096 || style.right > 4096 ||
        style.start < tocEnd)
      return false;
    style.glyphStart = uint64_t(style.start) + uint64_t(style.intervals) * 12;
    style.bitmapStart = style.glyphStart + uint64_t(style.glyphs) * 16 + uint64_t(style.left + style.right) * 3 +
                        uint64_t(style.leftClasses) * style.rightClasses + uint64_t(style.ligatures) * 8;
    return style.bitmapStart <= length;
  }
  bool validateIntervals() {
    uint32_t expected = 0, previous = 0;
    for (uint32_t index = 0; index < style.intervals; ++index) {
      if (!read(uint64_t(style.start) + uint64_t(index) * 12, 12)) return false;
      const auto first = number(0, 4), last = number(4, 4);
      if (first > last || last > 0x10ffff || (index && first <= previous) || number(8, 4) != expected ||
          expected > style.glyphs || last - first + 1 > style.glyphs - expected)
        return false;
      expected += last - first + 1;
      previous = last;
    }
    return expected == style.glyphs;
  }
  bool validateGlyphs() {
    for (uint32_t index = 0; index < style.glyphs; ++index) {
      if (!read(style.glyphStart + uint64_t(index) * 16, 16)) return false;
      const auto size = number(8, 2), offset = number(12, 4);
      const uint32_t required = (uint32_t(scratch[0]) * scratch[1] * bits + 7) / 8;
      if (size != required || offset > length - style.bitmapStart || size > length - style.bitmapStart - offset)
        return false;
    }
    return true;
  }
  bool covers(uint32_t codepoint) {
    uint32_t low = 0, high = style.intervals;
    while (low < high) {
      const auto middle = low + (high - low) / 2;
      if (!read(uint64_t(style.start) + uint64_t(middle) * 12, 12)) return false;
      if (number(4, 4) < codepoint)
        low = middle + 1;
      else
        high = middle;
    }
    return low < style.intervals && read(uint64_t(style.start) + uint64_t(low) * 12, 12) && number(0, 4) <= codepoint &&
           codepoint <= number(4, 4);
  }
  bool validateKern(uint64_t start, uint16_t count, uint8_t classes) {
    uint32_t previous = 0;
    for (uint32_t index = 0; index < count; ++index) {
      if (!read(start + uint64_t(index) * 3, 3)) return false;
      const auto codepoint = number(0, 2);
      const auto group = scratch[2];
      if ((index && codepoint <= previous) || group == 0 || group > classes || !covers(codepoint)) return false;
      previous = codepoint;
    }
    return true;
  }
  bool validateLigatures() {
    uint32_t previous = 0;
    const auto start = style.bitmapStart - uint64_t(style.ligatures) * 8;
    for (unsigned index = 0; index < style.ligatures; ++index) {
      if (!read(start + uint64_t(index) * 8, 8)) return false;
      const auto pair = number(0, 4), replacement = number(4, 4);
      if ((index && pair <= previous) || !covers(pair >> 16) || !covers(pair & 0xffff) || !covers(replacement))
        return false;
      previous = pair;
    }
    return true;
  }
};
}  // namespace companion

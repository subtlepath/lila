#pragma once

#include <cstdint>
#include <string_view>

namespace hal_filename {
using Fold = uint32_t (*)(uint32_t);
using Sink = bool (*)(void*, const uint8_t*, size_t);
enum class Comparison { Equal, Different, Invalid };

// Advances only after a complete, shortest-form Unicode scalar is decoded.
inline bool next(std::string_view& remaining, uint32_t& codepoint) {
  if (remaining.empty()) return false;
  const auto first = static_cast<uint8_t>(remaining.front());
  unsigned count = 1;
  uint32_t value = first;
  uint32_t minimum = 0;
  if (first >= 0xc2 && first <= 0xdf) {
    count = 2;
    value = first & 0x1f;
    minimum = 0x80;
  } else if (first >= 0xe0 && first <= 0xef) {
    count = 3;
    value = first & 0x0f;
    minimum = 0x800;
  } else if (first >= 0xf0 && first <= 0xf4) {
    count = 4;
    value = first & 0x07;
    minimum = 0x10000;
  } else if (first >= 0x80) {
    return false;
  }
  if (remaining.size() < count) return false;
  for (unsigned at = 1; at < count; ++at) {
    const auto byte = static_cast<uint8_t>(remaining[at]);
    if ((byte & 0xc0) != 0x80) return false;
    value = (value << 6) | (byte & 0x3f);
  }
  if (value == 0 || value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
  remaining.remove_prefix(count);
  codepoint = value;
  return true;
}

inline bool valid(std::string_view name) {
  uint32_t codepoint = 0;
  while (!name.empty())
    if (!next(name, codepoint)) return false;
  return true;
}

// Validate both complete inputs even when an earlier codepoint differs.
inline Comparison compare(std::string_view left, std::string_view right, Fold fold) {
  if (!fold) return Comparison::Invalid;
  bool equal = true;
  while (!left.empty() || !right.empty()) {
    const bool hasLeft = !left.empty();
    const bool hasRight = !right.empty();
    uint32_t a = 0, b = 0;
    if ((hasLeft && !next(left, a)) || (hasRight && !next(right, b))) return Comparison::Invalid;
    if (hasLeft != hasRight || fold(a) != fold(b)) equal = false;
  }
  return equal ? Comparison::Equal : Comparison::Different;
}

// ASCII lowercase preserves existing path hashes; other scalars use filesystem folding.
inline bool writeFolded(std::string_view name, Fold fold, Sink sink, void* context) {
  if (!fold || !sink) return false;
  while (!name.empty()) {
    uint32_t value = 0;
    if (!next(name, value)) return false;
    value = fold(value);
    if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
    if (value == 0 || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    uint8_t bytes[4];
    size_t count = 0;
    if (value < 0x80) {
      bytes[count++] = value;
    } else if (value < 0x800) {
      bytes[count++] = 0xc0 | (value >> 6);
      bytes[count++] = 0x80 | (value & 0x3f);
    } else if (value < 0x10000) {
      bytes[count++] = 0xe0 | (value >> 12);
      bytes[count++] = 0x80 | ((value >> 6) & 0x3f);
      bytes[count++] = 0x80 | (value & 0x3f);
    } else {
      bytes[count++] = 0xf0 | (value >> 18);
      bytes[count++] = 0x80 | ((value >> 12) & 0x3f);
      bytes[count++] = 0x80 | ((value >> 6) & 0x3f);
      bytes[count++] = 0x80 | (value & 0x3f);
    }
    if (!sink(context, bytes, count)) return false;
  }
  return true;
}
}  // namespace hal_filename

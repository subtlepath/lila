#pragma once

#include <cstddef>
#include <cstdint>

namespace tinta::core::utf8 {

inline constexpr uint32_t kReplacement = 0xFFFD;

constexpr bool isContinuation(uint8_t byte) { return (byte & 0xC0) == 0x80; }

// Decodes the codepoint at s[0..len) into `cp` and returns the bytes it spans
// (0 only when len is 0). Malformed input — a stray continuation byte, an
// overlong form, a surrogate, a value above U+10FFFF or a truncated sequence —
// decodes as kReplacement and consumes one byte, so a scan always makes
// progress and resynchronises at the next lead byte.
constexpr size_t decode(const char* s, size_t len, uint32_t& cp) {
  if (len == 0) {
    cp = kReplacement;
    return 0;
  }
  const uint8_t b0 = static_cast<uint8_t>(s[0]);
  if (b0 < 0x80) {
    cp = b0;
    return 1;
  }
  size_t need = 0;
  uint32_t value = 0;
  uint32_t min = 0;
  if ((b0 & 0xE0) == 0xC0) {
    need = 2;
    value = b0 & 0x1Fu;
    min = 0x80;
  } else if ((b0 & 0xF0) == 0xE0) {
    need = 3;
    value = b0 & 0x0Fu;
    min = 0x800;
  } else if ((b0 & 0xF8) == 0xF0) {
    need = 4;
    value = b0 & 0x07u;
    min = 0x10000;
  } else {
    cp = kReplacement;
    return 1;
  }
  if (len < need) {
    cp = kReplacement;
    return 1;
  }
  for (size_t i = 1; i < need; ++i) {
    const uint8_t b = static_cast<uint8_t>(s[i]);
    if (!isContinuation(b)) {
      cp = kReplacement;
      return 1;
    }
    value = (value << 6) | (b & 0x3Fu);
  }
  if (value < min || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
    cp = kReplacement;
    return 1;
  }
  cp = value;
  return need;
}

// Bytes encode() writes for `cp`.
constexpr size_t encodedLength(uint32_t cp) {
  if (cp < 0x80) return 1;
  if (cp < 0x800) return 2;
  if (cp < 0x10000 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 3;
  return 4;
}

// Writes `cp` to out (room for four bytes) and returns the bytes written.
// Surrogates and values above U+10FFFF are written as kReplacement.
constexpr size_t encode(uint32_t cp, char* out) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

// Index of the codepoint boundary after index i (len when i is at or past it).
constexpr size_t next(const char* s, size_t len, size_t i) {
  if (i >= len) return len;
  uint32_t cp = 0;
  return i + decode(s + i, len - i, cp);
}

// Index of the codepoint boundary before index i, agreeing with decode(): the
// bytes of a malformed sequence are one codepoint each.
constexpr size_t previous(const char* s, size_t i) {
  if (i == 0) return 0;
  size_t start = i - 1;
  while (start > 0 && i - start < 4 && isContinuation(static_cast<uint8_t>(s[start]))) --start;
  uint32_t cp = 0;
  return start + decode(s + start, i - start, cp) == i ? start : i - 1;
}

// Number of codepoints in s[0..len).
constexpr size_t count(const char* s, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i < len; i = next(s, len, i)) ++n;
  return n;
}

}  // namespace tinta::core::utf8

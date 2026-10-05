#pragma once

// The font character set (PLAN.md section 5.4), twin of tools/charset.py.
// Strings from the pack are already in slot form; this serves the few strings
// built at runtime. test/host/charset_test.cpp checks it against the Python.

#include <cstddef>
#include <cstdint>

#include "core/lang/Utf8.h"

// Slot spellings for string literals in C++ UI code, e.g. TINTA_EM_DASH "Sí"
// for "—Sí". Keep them as separate literals: "\xC2\x97" "a" concatenates
// safely, "\xC2\x97a" would not.
#define TINTA_EURO "\xC2\x80"
#define TINTA_LSQUO "\xC2\x91"
#define TINTA_RSQUO "\xC2\x92"
#define TINTA_LDQUO "\xC2\x93"
#define TINTA_RDQUO "\xC2\x94"
#define TINTA_BULLET "\xC2\x95"
#define TINTA_EN_DASH "\xC2\x96"
#define TINTA_EM_DASH "\xC2\x97"

namespace tinta::core::charset {

inline constexpr uint32_t kFirst = 0x20;
inline constexpr uint32_t kLast = 0xFF;

// DisplayTarget draws U+2026 as three full stops, so it stays in text as is.
inline constexpr uint32_t kEllipsis = 0x2026;

struct Remap {
  uint32_t unicode;
  uint8_t slot;
};

// Typographic punctuation lives in the unused C1 slots at its Windows-1252
// position.
inline constexpr Remap kRemap[] = {
    {0x20AC, 0x80},  // €
    {0x2018, 0x91},  // ‘
    {0x2019, 0x92},  // ’
    {0x201C, 0x93},  // “
    {0x201D, 0x94},  // ”
    {0x2022, 0x95},  // •
    {0x2013, 0x96},  // –
    {0x2014, 0x97},  // —
};

// The slot that draws `cp`, or -1 when it has none (U+2026 included: it is
// drawn without a slot; see isSupported()).
constexpr int32_t slotFor(uint32_t cp) {
  for (const Remap& r : kRemap) {
    if (r.unicode == cp) return r.slot;
  }
  if ((cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA0 && cp <= 0xFF)) return static_cast<int32_t>(cp);
  return -1;
}

// The Unicode codepoint whose glyph fills `slot`, or 0 for an empty slot.
constexpr uint32_t slotSource(uint32_t slot) {
  for (const Remap& r : kRemap) {
    if (r.slot == slot) return r.unicode;
  }
  if ((slot >= 0x20 && slot <= 0x7E) || (slot >= 0xA0 && slot <= 0xFF)) return slot;
  return 0;
}

// True when the fonts can draw `cp` (newline is layout, not a glyph).
constexpr bool isSupported(uint32_t cp) { return cp == kEllipsis || slotFor(cp) >= 0; }

// Rewrites NFC UTF-8 text into slot form: remapped characters become their
// slot codepoints; newlines and everything else supported pass through. The
// output is never longer than the input, so `out` may be `in`. Returns the
// bytes written, or -1 if the text has an unsupported or malformed character
// or does not fit `cap`.
inline int32_t encode(const char* in, size_t len, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < len;) {
    uint32_t cp = 0;
    const size_t n = utf8::decode(in + i, len - i, cp);
    i += n;
    if (cp != '\n' && !isSupported(cp)) return -1;  // U+FFFD (malformed) included
    const uint32_t outCp = cp == '\n' || cp == kEllipsis ? cp : static_cast<uint32_t>(slotFor(cp));
    if (o + utf8::encodedLength(outCp) > cap) return -1;
    o += utf8::encode(outCp, out + o);
  }
  return static_cast<int32_t>(o);
}

}  // namespace tinta::core::charset

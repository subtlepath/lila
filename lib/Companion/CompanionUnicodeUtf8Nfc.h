#pragma once

#include "CompanionUnicodeNfc.h"

namespace companion {
// All spans must be disjoint. Scalar banks are disposable scratch; UTF-8 output
// and count remain unchanged on failure. No NUL terminator is appended.
class UnicodeUtf8Nfc final {
 public:
  static bool normalize(std::span<const uint8_t> input, std::span<uint32_t> decoded, std::span<uint32_t> normalized,
                        std::span<uint8_t> output, size_t& outputBytes) {
    size_t scalars = 0;
    uint32_t scalar = 0, minimum = 0;
    unsigned remaining = 0;
    for (const auto byte : input) {
      if (remaining) {
        if ((byte & 0xc0) != 0x80) return false;
        scalar = (scalar << 6) | (byte & 63);
        if (--remaining) continue;
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
      } else if (byte < 128)
        scalar = byte;
      else if (byte >= 0xc2 && byte <= 0xdf) {
        scalar = byte & 31;
        minimum = 128;
        remaining = 1;
        continue;
      } else if (byte >= 0xe0 && byte <= 0xef) {
        scalar = byte & 15;
        minimum = 2048;
        remaining = 2;
        continue;
      } else if (byte >= 0xf0 && byte <= 0xf4) {
        scalar = byte & 7;
        minimum = 65536;
        remaining = 3;
        continue;
      } else
        return false;
      if (scalars == decoded.size()) return false;
      decoded[scalars++] = scalar;
    }
    size_t count = 0;
    if (remaining || !UnicodeNfc::normalize(decoded.first(scalars), normalized, count)) return false;
    size_t required = 0;
    for (const auto cp : normalized.first(count)) {
      const size_t width = cp < 128 ? 1 : cp < 2048 ? 2 : cp < 65536 ? 3 : 4;
      if (required > output.size() || width > output.size() - required) return false;
      required += width;
    }
    size_t at = 0;
    for (const auto cp : normalized.first(count)) {
      if (cp < 128)
        output[at++] = cp;
      else {
        if (cp < 2048)
          output[at++] = 0xc0 | (cp >> 6);
        else {
          if (cp < 65536)
            output[at++] = 0xe0 | (cp >> 12);
          else {
            output[at++] = 0xf0 | (cp >> 18);
            output[at++] = 0x80 | ((cp >> 12) & 63);
          }
          output[at++] = 0x80 | ((cp >> 6) & 63);
        }
        output[at++] = 0x80 | (cp & 63);
      }
    }
    outputBytes = required;
    return true;
  }
};
}  // namespace companion

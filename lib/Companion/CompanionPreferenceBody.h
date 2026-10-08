#pragma once

#include <algorithm>
#include <bit>

#include "CompanionRecords.h"

namespace companion {
inline constexpr Digest PREFERENCE_SCOPE{0x29, 0x40, 0x31, 0x20, 0xa7, 0xc2, 0x8b, 0x97, 0x62, 0xea, 0x6e,
                                         0xcb, 0x7f, 0x8a, 0xa4, 0x63, 0x31, 0x61, 0x9e, 0x04, 0x38, 0xa4,
                                         0x96, 0xf3, 0x03, 0x8b, 0x4f, 0x32, 0xbd, 0x12, 0x1e, 0x79};
// Views borrow the encoded body; they are not null-terminated.
struct PreferenceBodyView {
  uint8_t key = 0, type = 0;
  int32_t integer = 0;
  bool hasContent = false;
  std::span<const uint8_t> text, contentHash;
};
inline bool validPreferenceInteger(uint8_t key, int32_t value) {
  switch (key) {
    case 1:
      return value >= 0 && value <= 1;
    case 2:
      return value >= 1 && value <= 255;
    case 3:
      return value >= 0 && value <= 3;
    case 4:
      return value >= 0 && value <= 4;
    case 5:
    case 9:
    case 12:
    case 13:
    case 14:
    case 39:
    case 40:
      return value >= 0 && value <= 1;
    case 6:
      return value >= 50 && value <= 200 && value % 25 == 0;
    case 7:
      return value >= -2 && value <= 2;
    case 8:
      return value >= 5 && value <= 40 && value % 5 == 0;
    case 32:
      return value >= 0 && value <= 200;
    case 33:
      return value >= 0 && value <= 9999;
    case 34:
      return value >= 700 && value <= 970;
    case 35:
      return value >= 1 && value <= 36500;
    case 36:
      return value >= 5 && value <= 500;
    case 37:
    case 38:
      return value >= 0 && value <= 2;
    default:
      return false;
  }
}
inline bool validPreferenceUtf8(std::span<const uint8_t> bytes) {
  uint32_t scalar = 0, minimum = 0;
  unsigned remaining = 0;
  for (const auto byte : bytes) {
    if (remaining) {
      if ((byte & 0xc0) != 0x80) return false;
      scalar = (scalar << 6) | (byte & 63);
      if (--remaining) continue;
      if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
    } else if (byte < 128) {
      continue;
    } else if (byte >= 0xc2 && byte <= 0xdf) {
      scalar = byte & 31;
      minimum = 128;
      remaining = 1;
    } else if (byte >= 0xe0 && byte <= 0xef) {
      scalar = byte & 15;
      minimum = 2048;
      remaining = 2;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
      scalar = byte & 7;
      minimum = 65536;
      remaining = 3;
    } else
      return false;
  }
  return remaining == 0;
}
inline bool decodePreferenceBody(std::span<const uint8_t> bytes, PreferenceBodyView& output) {
  if (bytes.size() < 5 || bytes.size() > 69 || bytes[0] != 1 || bytes[1] != static_cast<uint8_t>(EventKind::Preference))
    return false;
  PreferenceBodyView value;
  value.key = bytes[2];
  value.type = bytes[3];
  switch (value.type) {
    case 1: {
      if (bytes.size() != 8) return false;
      uint32_t number = 0;
      for (unsigned i = 0; i < 4; ++i) number |= static_cast<uint32_t>(bytes[4 + i]) << (8 * i);
      value.integer = std::bit_cast<int32_t>(number);
      if (!validPreferenceInteger(value.key, value.integer)) return false;
      break;
    }
    case 2: {
      if (value.key != 10 || bytes[4] == 0 || bytes[4] > 63 || bytes.size() != size_t{5} + bytes[4]) return false;
      value.text = bytes.subspan(5);
      unsigned component = 0, length = 0;
      for (const auto byte : value.text) {
        if (byte == '-') {
          if (length == 0) return false;
          ++component;
          length = 0;
          continue;
        }
        if (++length > 8 || !((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                              (component && byte >= '0' && byte <= '9')))
          return false;
      }
      if (length == 0) return false;
      break;
    }
    case 3: {
      if ((value.key != 1 && value.key != 11) || bytes[4] > 1) return false;
      value.hasContent = bytes[4] == 1;
      if (!value.hasContent) {
        if (value.key != 11 || bytes.size() != 5) return false;
        break;
      }
      if (bytes.size() < 39 || bytes[37] == 0 || bytes[37] > 31 || bytes.size() != size_t{38} + bytes[37]) return false;
      value.contentHash = bytes.subspan(5, 32);
      if (std::none_of(value.contentHash.begin(), value.contentHash.end(), [](uint8_t byte) { return byte != 0; }))
        return false;
      value.text = bytes.subspan(38);
      if ((value.text.size() == 1 && value.text[0] == '.') ||
          (value.text.size() == 2 && value.text[0] == '.' && value.text[1] == '.'))
        return false;
      if (!validPreferenceUtf8(value.text) || std::any_of(value.text.begin(), value.text.end(), [](uint8_t byte) {
            return byte < 32 || byte == 127 || byte == '/' || byte == '\\' || byte == ':';
          }))
        return false;
      break;
    }
    default:
      return false;
  }
  output = value;
  return true;
}
}  // namespace companion

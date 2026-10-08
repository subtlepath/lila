#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace BookmarkIdentity {
using Value = std::array<uint8_t, 16>;
inline bool valid(const Value& identity) {
  return std::any_of(identity.begin(), identity.end(), [](uint8_t byte) { return byte != 0; });
}
inline bool decode(std::string_view text, Value& output) {
  if (text.size() != 32) return false;
  Value candidate{};
  for (size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    const int nibble = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
    if (nibble < 0) return false;
    candidate[i / 2] |= static_cast<uint8_t>(nibble << (i % 2 == 0 ? 4 : 0));
  }
  if (!valid(candidate)) return false;
  output = candidate;
  return true;
}
inline bool encode(const Value& identity, std::span<char> output) {
  if (!valid(identity) || output.size() < 33) return false;
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < identity.size(); ++i) {
    output[2 * i] = HEX_DIGITS[identity[i] >> 4];
    output[2 * i + 1] = HEX_DIGITS[identity[i] & 15];
  }
  output[32] = '\0';
  return true;
}
}  // namespace BookmarkIdentity

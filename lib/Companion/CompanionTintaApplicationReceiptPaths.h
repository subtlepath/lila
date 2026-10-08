#pragma once

#include <string_view>

#include "CompanionTintaBody.h"

namespace companion {
inline constexpr size_t TINTA_APPLICATION_ADDRESS_SIZE = 100;
inline constexpr std::string_view TINTA_APPLICATION_PATH_PREFIX = "/.crosspoint/companion/tinta-applied-";
inline constexpr size_t TINTA_APPLICATION_PATH_SIZE = TINTA_APPLICATION_PATH_PREFIX.size() + 64 + 5;
// Hash this canonical tuple, not the mutable before/after images of the receipt.
inline bool encodeTintaApplicationAddress(const EventIdentity& event, const Identity& course,
                                          const Identity& generation, const Digest& resource,
                                          std::span<uint8_t> output) {
  if (output.size() < TINTA_APPLICATION_ADDRESS_SIZE || !tinta_body_detail::validIdentity(event) ||
      !tinta_body_detail::nonzero(course) || !tinta_body_detail::nonzero(generation) ||
      !tinta_body_detail::nonzero(resource))
    return false;
  std::array<uint8_t, TINTA_APPLICATION_ADDRESS_SIZE> bytes{};
  std::copy_n("TAA\1", 4, bytes.begin());
  std::copy(event.origin.begin(), event.origin.end(), bytes.begin() + 4);
  tinta_body_detail::write(bytes, 20, event.epoch, 8);
  tinta_body_detail::write(bytes, 28, event.sequence, 8);
  std::copy(course.begin(), course.end(), bytes.begin() + 36);
  std::copy(generation.begin(), generation.end(), bytes.begin() + 52);
  std::copy(resource.begin(), resource.end(), bytes.begin() + 68);
  std::copy(bytes.begin(), bytes.end(), output.begin());
  return true;
}
inline bool tintaApplicationReceiptPath(const Digest& addressHash, bool stage, std::span<char> output) {
  if (output.size() < TINTA_APPLICATION_PATH_SIZE || !tinta_body_detail::nonzero(addressHash)) return false;
  const auto digest = addressHash;
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  std::copy(TINTA_APPLICATION_PATH_PREFIX.begin(), TINTA_APPLICATION_PATH_PREFIX.end(), output.begin());
  size_t at = TINTA_APPLICATION_PATH_PREFIX.size();
  for (const auto byte : digest) {
    output[at++] = HEX_DIGITS[byte >> 4];
    output[at++] = HEX_DIGITS[byte & 15];
  }
  if (stage) {
    std::copy_n(".tmp", 4, output.begin() + at);
    at += 4;
  }
  output[at] = 0;
  return true;
}
}  // namespace companion

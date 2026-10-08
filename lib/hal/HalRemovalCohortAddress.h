#pragma once

#include <Logging.h>
#include <mbedtls/sha256.h>

#include "CompanionRecords.h"

namespace companion {
// Address derivation is not authorization: the caller verifies the parent plan
// and ordinal, then retains the parent digest for journal ownership.
inline bool removalCohortAddress(mbedtls_sha256_context& hash, const Digest& parent, uint64_t ordinal,
                                 std::span<char> output) {
  static constexpr char DOMAIN[] = "lila/removal-cohort-bytes/v1";
  static constexpr char PREFIX[] = "/.crosspoint/companion/removal-cohort-bytes-";
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  if (output.size() < sizeof(PREFIX) + 64 ||
      !std::any_of(parent.begin(), parent.end(), [](uint8_t byte) { return byte != 0; })) {
    LOG_ERR("COMPANION", "Invalid removal cohort address arguments");
    return false;
  }
  std::array<uint8_t, 8> encoded{};
  for (unsigned at = 0; at < 8; ++at) encoded[at] = static_cast<uint8_t>(ordinal >> (at * 8));
  Digest digest{};
  if (mbedtls_sha256_starts(&hash, 0) ||
      mbedtls_sha256_update(&hash, reinterpret_cast<const uint8_t*>(DOMAIN), sizeof(DOMAIN) - 1) ||
      mbedtls_sha256_update(&hash, parent.data(), parent.size()) ||
      mbedtls_sha256_update(&hash, encoded.data(), encoded.size()) || mbedtls_sha256_finish(&hash, digest.data())) {
    LOG_ERR("COMPANION", "Removal cohort address SHA failed");
    return false;
  }
  size_t at = sizeof(PREFIX) - 1;
  std::copy_n(PREFIX, at, output.begin());
  for (uint8_t byte : digest) {
    output[at++] = HEX_DIGITS[byte >> 4];
    output[at++] = HEX_DIGITS[byte & 15];
  }
  output[at] = 0;
  return true;
}
}  // namespace companion

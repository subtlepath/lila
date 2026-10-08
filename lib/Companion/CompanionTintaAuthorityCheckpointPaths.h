#pragma once

#include <string_view>

#include "CompanionTintaBody.h"

namespace companion {
inline constexpr std::string_view TINTA_CHECKPOINT_PATH_PREFIX = "/.crosspoint/companion/tinta-checkpoint-";
inline constexpr size_t TINTA_CHECKPOINT_PATH_SIZE = TINTA_CHECKPOINT_PATH_PREFIX.size() + 64 + 5;
inline bool tintaAuthorityCheckpointPath(const Digest& manifestHash, bool stage, std::span<char> output) {
  if (output.size() < TINTA_CHECKPOINT_PATH_SIZE || !tinta_body_detail::nonzero(manifestHash)) return false;
  const auto digest = manifestHash;
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  std::copy(TINTA_CHECKPOINT_PATH_PREFIX.begin(), TINTA_CHECKPOINT_PATH_PREFIX.end(), output.begin());
  size_t at = TINTA_CHECKPOINT_PATH_PREFIX.size();
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

#pragma once

#include "CompanionRemovalMetadataSnapshot.h"

namespace companion {
inline bool removalMetadataPaths(const Digest& plan, RemovalMetadataFile kind, std::array<char, 112>& candidate,
                                 std::array<char, 112>& backup) {
  if (!removalMetadataDigestNonzero(plan) ||
      (kind != RemovalMetadataFile::State && kind != RemovalMetadataFile::Recent))
    return false;
  static constexpr char PREFIX[] = "/.crosspoint/companion/removal-meta-";
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  static_assert(sizeof(PREFIX) + 64 + 1 + 5 <= 112);
  size_t at = sizeof(PREFIX) - 1;
  std::copy_n(PREFIX, at, candidate.begin());
  for (const auto byte : plan) {
    candidate[at++] = HEX_DIGITS[byte >> 4];
    candidate[at++] = HEX_DIGITS[byte & 15];
  }
  candidate[at++] = kind == RemovalMetadataFile::State ? 's' : 'r';
  std::copy_n(candidate.begin(), at, backup.begin());
  std::copy_n(".next", 6, candidate.begin() + at);
  std::copy_n(".old", 5, backup.begin() + at);
  return true;
}
}  // namespace companion

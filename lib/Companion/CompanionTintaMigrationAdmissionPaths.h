#pragma once

#include <string_view>
#include <utility>

#include "CompanionTintaMigrationAdmission.h"

namespace companion {
inline constexpr size_t TINTA_MIGRATION_ADDRESS_SIZE = 16 + JOURNAL_MERGE_INTENT_SIZE;
inline constexpr std::string_view TINTA_MIGRATION_PATH_PREFIX = "/.crosspoint/companion/tinta-migration-";
inline constexpr size_t TINTA_MIGRATION_PATH_SIZE = TINTA_MIGRATION_PATH_PREFIX.size() + 64 + 5;
inline bool encodeTintaMigrationAddress(const Identity& course, const JournalMergeIntent& merge,
                                        std::span<uint8_t> output) {
  if (output.size() != TINTA_MIGRATION_ADDRESS_SIZE || !tinta_body_detail::nonzero(course) ||
      !validJournalMergeIntent(merge) || merge.merged.count <= merge.previous.count)
    return false;
  const auto begin = reinterpret_cast<uintptr_t>(output.data());
  for (const auto& range : {std::pair{reinterpret_cast<uintptr_t>(&course), sizeof(course)},
                            std::pair{reinterpret_cast<uintptr_t>(&merge), sizeof(merge)}})
    if (begin < range.first + range.second && range.first < begin + output.size()) return false;
  std::copy(course.begin(), course.end(), output.begin());
  return encodeJournalMergeIntent(merge, output.subspan(16));
}
inline bool tintaMigrationAdmissionPath(const Digest& address, bool stage, std::span<char> output) {
  if (output.size() < TINTA_MIGRATION_PATH_SIZE || !tinta_body_detail::nonzero(address)) return false;
  const auto digest = address;
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  std::copy(TINTA_MIGRATION_PATH_PREFIX.begin(), TINTA_MIGRATION_PATH_PREFIX.end(), output.begin());
  size_t at = TINTA_MIGRATION_PATH_PREFIX.size();
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

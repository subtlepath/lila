#pragma once

#include "CompanionJournalMergeIntent.h"

namespace companion {
inline constexpr size_t JOURNAL_MERGE_READINESS_REQUEST_SIZE = 60;
inline constexpr size_t JOURNAL_MERGE_READINESS_REPLY_SIZE = 8;
enum class JournalMergeReadiness : uint8_t { Ready, MigrationRequired, Unavailable, NoCourse };
inline bool decodeJournalMergeReadinessRequest(std::span<const uint8_t> bytes, Identity& generation,
                                               JournalMergeSnapshot& snapshot) {
  if (bytes.size() != JOURNAL_MERGE_READINESS_REQUEST_SIZE || bytes[0] != 'J' || bytes[1] != 'R' || bytes[2] != 'D' ||
      bytes[3] != 1 || bytes[26] != 0 || bytes[27] != 0)
    return false;
  Identity decodedGeneration{};
  JournalMergeSnapshot decoded;
  std::copy_n(bytes.begin() + 4, 16, decodedGeneration.begin());
  decoded.count = tinta_body_detail::read(bytes, 20, 4);
  decoded.recordSize = tinta_body_detail::read(bytes, 24, 2);
  std::copy_n(bytes.begin() + 28, 32, decoded.frontier.begin());
  if (!tinta_body_detail::nonzero(decodedGeneration) || !validJournalMergeSnapshot(decoded)) return false;
  generation = decodedGeneration;
  snapshot = decoded;
  return true;
}
inline bool encodeJournalMergeReadinessReply(JournalMergeReadiness result, std::span<uint8_t> bytes) {
  if (bytes.size() != JOURNAL_MERGE_READINESS_REPLY_SIZE || result > JournalMergeReadiness::NoCourse) return false;
  std::fill(bytes.begin(), bytes.end(), uint8_t{0});
  bytes[0] = 'J';
  bytes[1] = 'R';
  bytes[2] = 'R';
  bytes[3] = 1;
  bytes[4] = static_cast<uint8_t>(result);
  return true;
}
}  // namespace companion

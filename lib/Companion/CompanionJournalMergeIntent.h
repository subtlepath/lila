#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionJournalMergePublication.h"

namespace companion {
inline constexpr size_t JOURNAL_MERGE_INTENT_SIZE = 136;
inline bool encodeJournalMergeIntent(const JournalMergeIntent& intent, std::span<uint8_t> bytes) {
  if (bytes.size() != JOURNAL_MERGE_INTENT_SIZE || !validJournalMergeIntent(intent)) return false;
  std::fill(bytes.begin(), bytes.end(), 0);
  bytes[0] = 'J';
  bytes[1] = 'M';
  bytes[2] = 'P';
  bytes[3] = 1;
  std::copy(intent.generation.begin(), intent.generation.end(), bytes.begin() + 4);
  std::copy(intent.owner.begin(), intent.owner.end(), bytes.begin() + 20);
  std::copy(intent.transaction.begin(), intent.transaction.end(), bytes.begin() + 36);
  tinta_body_detail::write(bytes, 52, intent.previous.count, 4);
  tinta_body_detail::write(bytes, 56, intent.previous.recordSize, 2);
  std::copy(intent.previous.frontier.begin(), intent.previous.frontier.end(), bytes.begin() + 60);
  tinta_body_detail::write(bytes, 92, intent.merged.count, 4);
  tinta_body_detail::write(bytes, 96, intent.merged.recordSize, 2);
  std::copy(intent.merged.frontier.begin(), intent.merged.frontier.end(), bytes.begin() + 100);
  tinta_body_detail::write(bytes, 132, binary_record::crc32(bytes.data(), 132), 4);
  return true;
}
inline bool decodeJournalMergeIntent(std::span<const uint8_t> bytes, JournalMergeIntent& output) {
  if (bytes.size() != JOURNAL_MERGE_INTENT_SIZE || bytes[0] != 'J' || bytes[1] != 'M' || bytes[2] != 'P' ||
      bytes[3] != 1 || bytes[58] != 0 || bytes[59] != 0 || bytes[98] != 0 || bytes[99] != 0 ||
      tinta_body_detail::read(bytes, 132, 4) != binary_record::crc32(bytes.data(), 132))
    return false;
  JournalMergeIntent decoded;
  std::copy_n(bytes.begin() + 4, 16, decoded.generation.begin());
  std::copy_n(bytes.begin() + 20, 16, decoded.owner.begin());
  std::copy_n(bytes.begin() + 36, 16, decoded.transaction.begin());
  decoded.previous.count = tinta_body_detail::read(bytes, 52, 4);
  decoded.previous.recordSize = tinta_body_detail::read(bytes, 56, 2);
  std::copy_n(bytes.begin() + 60, 32, decoded.previous.frontier.begin());
  decoded.merged.count = tinta_body_detail::read(bytes, 92, 4);
  decoded.merged.recordSize = tinta_body_detail::read(bytes, 96, 2);
  std::copy_n(bytes.begin() + 100, 32, decoded.merged.frontier.begin());
  if (!validJournalMergeIntent(decoded)) return false;
  output = decoded;
  return true;
}
}  // namespace companion

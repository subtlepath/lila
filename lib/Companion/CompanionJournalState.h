#pragma once

#include <cstring>

#include "CompanionJournalMergeIntent.h"

namespace companion {
inline constexpr size_t JOURNAL_STATE_REQUEST_SIZE = 20;
inline constexpr size_t JOURNAL_STATE_REPLY_SIZE = 44;
inline bool decodeJournalStateRequest(std::span<const uint8_t> bytes, Identity& generation) {
  if (bytes.size() != JOURNAL_STATE_REQUEST_SIZE || bytes[0] != 'J' || bytes[1] != 'S' || bytes[2] != 'T' ||
      bytes[3] != 1 || !std::any_of(bytes.begin() + 4, bytes.end(), [](uint8_t value) { return value != 0; }))
    return false;
  memmove(generation.data(), bytes.data() + 4, generation.size());
  return true;
}
inline bool encodeJournalStateReply(const JournalMergeSnapshot& snapshot, std::span<uint8_t> output) {
  if (output.size() != JOURNAL_STATE_REPLY_SIZE || !validJournalMergeSnapshot(snapshot)) return false;
  const auto first = reinterpret_cast<uintptr_t>(output.data()), second = reinterpret_cast<uintptr_t>(&snapshot);
  if (first >= second ? first - second < sizeof(snapshot) : second - first < output.size()) return false;
  output[0] = 'J';
  output[1] = 'S';
  output[2] = 'S';
  output[3] = 1;
  tinta_body_detail::write(output, 4, snapshot.count, 4);
  tinta_body_detail::write(output, 8, snapshot.recordSize, 2);
  output[10] = output[11] = 0;
  std::copy(snapshot.frontier.begin(), snapshot.frontier.end(), output.begin() + 12);
  return true;
}
}  // namespace companion

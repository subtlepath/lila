#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "core/session/SessionFile.h"

namespace companion {
struct LegacySessionView {
  tinta::core::SessionFileView file;
  std::span<const uint8_t> entries;
  uint32_t journalCount = 0;
  uint16_t queued = 0, day = 0, tag = 0;
  uint8_t kind = 0;
};
// Borrows immutable reviewed bytes. Screen/capacity bounds come from the native
// app. This validates storage syntax without restoring or changing a session.
inline bool decodeTintaLegacySession(std::span<const uint8_t> bytes, uint8_t maxDepth, uint8_t screenCount,
                                     uint16_t maxQueued, LegacySessionView& output) {
  LegacySessionView value;
  if (!tinta::core::decodeSessionFile(bytes, maxDepth, screenCount, value.file)) return false;
  const auto session = value.file.session;
  if (session.empty()) {
    output = value;
    return true;
  }
  if (session.size() < 20 || session[4] > 1 || session[5] || binary_record::getU16(session.data() + 18) ||
      binary_record::getU16(session.data() + 8) > binary_record::getU16(session.data() + 6) ||
      binary_record::getU16(session.data() + 10) > binary_record::getU16(session.data() + 6))
    return false;
  const auto length = binary_record::getU16(session.data() + 16);
  if (session.size() != 20u + length || length < 28) return false;
  const auto queue = session.subspan(20);
  if (!std::equal(queue.begin(), queue.begin() + 4, "TSQ1")) return false;
  const auto version = binary_record::getU16(queue.data() + 4);
  if (version != 1 && version != 2) return false;
  const size_t header = version == 1 ? 24 : 28;
  value.queued = binary_record::getU16(queue.data() + 6);
  if (value.queued > maxQueued || queue.size() != header + size_t(value.queued) * 5 + 4 ||
      binary_record::getU32(queue.data() + queue.size() - 4) != binary_record::crc32(queue.data(), queue.size() - 4) ||
      queue[19] || (version == 1 && queue[18]) ||
      binary_record::getU16(queue.data() + 14) > binary_record::getU16(queue.data() + 12) ||
      binary_record::getU16(queue.data() + 16) > binary_record::getU16(queue.data() + 12))
    return false;
  if (version == 2) {
    if (queue[18] > 1 || binary_record::getU16(queue.data() + 26)) return false;
    value.kind = queue[18];
    value.tag = binary_record::getU16(queue.data() + 24);
  }
  value.entries = queue.subspan(header, size_t(value.queued) * 5);
  for (size_t at = 0; at < value.entries.size(); at += 5) {
    const auto uid = binary_record::getU32(value.entries.data() + at);
    if (!uid || uid == UINT32_MAX || value.entries[at + 4] & ~3u) return false;
  }
  value.journalCount = binary_record::getU32(session.data());
  value.day = binary_record::getU16(queue.data() + 8);
  output = value;
  return true;
}
}  // namespace companion

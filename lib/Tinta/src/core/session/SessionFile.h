#pragma once

#include <algorithm>
#include <span>

#include "core/srs/Bytes.h"

namespace tinta::core {
struct SessionFileView {
  uint16_t version = 0;
  std::span<const uint8_t> screens, session, snapshot;
};
// Trailing bytes are allowed: session.bin is rewritten in place after grades.
inline bool decodeSessionFile(std::span<const uint8_t> bytes, uint8_t maxDepth, uint8_t screenCount,
                              SessionFileView& output) {
  if (bytes.size() < 11 || !std::equal(bytes.begin(), bytes.begin() + 4, "TSES")) return false;
  SessionFileView value;
  value.version = getU16(bytes.data() + 4);
  const auto depth = bytes[6];
  if (value.version < 1 || value.version > 3 || depth > maxDepth || size_t{7} + depth + 4 > bytes.size()) return false;
  value.screens = bytes.subspan(7, depth);
  for (const auto screen : value.screens)
    if (!screen || screen >= screenCount) return false;
  size_t covered = 7 + depth;
  if (value.version >= 2) {
    if (covered + 2 > bytes.size()) return false;
    const auto length = getU16(bytes.data() + covered);
    covered += 2;
    if (length > bytes.size() - covered) return false;
    value.session = bytes.subspan(covered, length);
    covered += length;
  }
  if (value.version >= 3) {
    if (covered + 32 + 4 > bytes.size()) return false;
    value.snapshot = bytes.subspan(covered, 32);
    if (std::all_of(value.snapshot.begin(), value.snapshot.end(), [](uint8_t byte) { return byte == 0; })) return false;
    covered += 32;
  }
  if (covered + 4 > bytes.size() || getU32(bytes.data() + covered) != crc32(bytes.data(), covered)) return false;
  output = value;
  return true;
}
inline bool sessionSnapshotChanged(const SessionFileView& saved, std::span<const uint8_t, 32> current) {
  return saved.snapshot.size() != current.size() || !std::equal(current.begin(), current.end(), saved.snapshot.begin());
}
}  // namespace tinta::core

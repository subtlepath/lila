#pragma once

#include <cstdint>

namespace companion {
// Offset is local minus UTC at the event's instant; caller verifies the clock and offset.
inline bool tintaLocalSecondsToUnixUtc(uint32_t localSeconds, int32_t utcOffsetSeconds, uint64_t& output) {
  static constexpr int64_t EPOCH_2024 = 1704067200;
  static constexpr int32_t MAX_OFFSET = 24 * 60 * 60;
  if (utcOffsetSeconds < -MAX_OFFSET || utcOffsetSeconds > MAX_OFFSET) return false;
  const int64_t normalized = EPOCH_2024 + static_cast<int64_t>(localSeconds) - utcOffsetSeconds;
  if (normalized < 0) return false;
  output = static_cast<uint64_t>(normalized);
  return true;
}
}  // namespace companion

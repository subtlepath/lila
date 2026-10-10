#pragma once

#include <cstring>

#include "HalTintaReplayStore.h"

namespace companion {
namespace replay_day_correspondence {
inline constexpr size_t COVERAGE_BYTES = (uint32_t(UINT16_MAX) + 1) / 8;
using Totals = std::array<uint32_t, 4>;
inline bool readRecord(HalFile& file, uint64_t offset, std::span<uint8_t, 12> bytes) {
  return file.seek64(offset) && file.read(bytes.data(), bytes.size()) == static_cast<int64_t>(bytes.size()) &&
         binary_record::getU16(bytes.data() + 10) == uint16_t(binary_record::crc32(bytes.data(), 10));
}
// Local day logs need not be sorted; aggregate without retaining their records.
[[gnu::noinline]] inline bool retainedTotals(HalFile& file, uint64_t length, uint16_t day, Totals& output,
                                             bool (*permitted)(void*), void* context) {
  Totals totals{};
  std::array<uint8_t, 12> bytes{};
  for (uint64_t at = 4; at < length; at += 12) {
    if ((at - 4) % (12 * 32) == 0) vTaskDelay(1);
    if (!permitted(context) || !readRecord(file, at, bytes)) return false;
    if (binary_record::getU16(bytes.data()) != day) continue;
    for (size_t i = 0; i < totals.size(); ++i) {
      const auto delta = binary_record::getU16(bytes.data() + 2 + i * 2);
      if (delta > UINT32_MAX - totals[i]) return false;
      totals[i] += delta;
    }
  }
  if (totals[1] > totals[0] || totals[2] > totals[0] || file.fileSize64() != length || !permitted(context))
    return false;
  output = totals;
  return true;
}
inline bool matches(HalTintaReplayStore& store, uint16_t day, const Totals& retained) {
  TintaReplayDay projected;
  if (!store.day(day, projected)) return false;
  const uint64_t seconds = projected.responseMilliseconds / 1000 + (projected.responseMilliseconds % 1000 >= 500);
  return projected.gradedReviews == retained[0] && projected.correctReviews == retained[1] &&
         projected.newItems == retained[2] && seconds == retained[3];
}
}  // namespace replay_day_correspondence
// Caller verifies immutable file/hash and freezes projection writers. This
// compares canonical replay totals; legacy session rounding may differ.
inline bool compareTintaReplayDays(HalFile& file, HalTintaReplayStore& store, const Identity& course,
                                   std::span<uint8_t> scratch, bool (*permitted)(void*), void* context) {
  const auto failure = []([[maybe_unused]] const char* reason) {
    LOG_ERR("COMPANION", "Replay day correspondence refused: %s", reason);
    return false;
  };
  if (!permitted || !permitted(context) || scratch.size() < replay_day_correspondence::COVERAGE_BYTES ||
      !file.isOpen() || file.isDirectory() || !store.matchesCourse(course))
    return failure("arguments or binding");
  const auto length = file.fileSize64();
  std::array<uint8_t, 12> bytes{};
  if (length < 4 || length > UINT32_MAX || (length - 4) % 12 || !file.seek64(0) || file.read(bytes.data(), 4) != 4 ||
      std::memcmp(bytes.data(), "TDL1", 4))
    return failure("extent or header");
  const auto seen = scratch.first(replay_day_correspondence::COVERAGE_BYTES);
  std::fill(seen.begin(), seen.end(), 0);
  for (uint64_t at = 4; at < length; at += 12) {
    if ((at - 4) % (12 * 32) == 0) vTaskDelay(1);
    if (!permitted(context) || !replay_day_correspondence::readRecord(file, at, bytes)) return failure("record");
    const auto day = binary_record::getU16(bytes.data());
    const auto mask = uint8_t(1u << (day % 8));
    if (seen[day / 8] & mask) continue;
    replay_day_correspondence::Totals totals{};
    if (!replay_day_correspondence::retainedTotals(file, length, day, totals, permitted, context) ||
        !replay_day_correspondence::matches(store, day, totals))
      return failure("day totals");
    seen[day / 8] |= mask;
  }
  uint32_t key = 0;
  bool previous = false, found = false;
  for (;;) {
    if (!permitted(context) || !store.nextKey(HalTintaReplayStore::Kind::Day, previous, key, key, found))
      return failure("projection enumeration");
    if (!found) break;
    if (key > UINT16_MAX || (!(seen[key / 8] & uint8_t(1u << (key % 8))) &&
                             !replay_day_correspondence::matches(store, static_cast<uint16_t>(key), {})))
      return failure("missing retained day");
    previous = true;
  }
  return (file.fileSize64() == length && permitted(context)) || failure("extent or permission");
}
}  // namespace companion

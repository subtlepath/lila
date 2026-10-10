#pragma once

#include <algorithm>
#include <array>
#include <cstring>

#include "HalUnboundCourseReviewedFile.h"

namespace companion {
struct UnboundCourseDayReport {
  std::array<uint32_t, 4> totals{};
  uint32_t records = 0, days = 0;
  bool present = false;
};
namespace unbound_course_detail {
inline constexpr size_t DAY_COVERAGE_BYTES = (uint32_t(UINT16_MAX) + 1) / 8;
inline bool readLegacyDay(HalFile& file, uint64_t offset, std::array<uint8_t, 12>& bytes, bool (*permitted)(void*),
                          void* context) {
  if ((offset - 4) % (12 * 32) == 0) vTaskDelay(1);
  return permitted(context) && file.seek64(offset) && file.read(bytes.data(), bytes.size()) == 12 &&
         binary_record::getU16(bytes.data() + 10) == uint16_t(binary_record::crc32(bytes.data(), 10));
}
inline bool addLegacyDayTotals(const std::array<uint8_t, 12>& bytes, std::array<uint32_t, 4>& totals) {
  for (size_t index = 0; index < totals.size(); ++index) {
    const auto delta = binary_record::getU16(bytes.data() + 2 + index * 2);
    if (delta > UINT32_MAX - totals[index]) return false;
    totals[index] += delta;
  }
  return true;
}
[[gnu::noinline]] inline bool inspectLegacyDayTotals(HalFile& file, uint64_t length, uint16_t day,
                                                     bool (*permitted)(void*), void* context) {
  std::array<uint32_t, 4> totals{};
  std::array<uint8_t, 12> bytes{};
  for (uint64_t offset = 4; offset < length; offset += 12) {
    if (!readLegacyDay(file, offset, bytes, permitted, context)) return false;
    if (binary_record::getU16(bytes.data()) == day && !addLegacyDayTotals(bytes, totals)) return false;
  }
  return totals[1] <= totals[0] && totals[2] <= totals[0] && file.fileSize64() == length && permitted(context);
}
[[gnu::noinline]] inline bool inspectLegacyDayFile(HalFile& file, std::span<uint8_t> scratch,
                                                   UnboundCourseDayReport& report, bool (*permitted)(void*),
                                                   void* context) {
  const auto length = file.fileSize64();
  std::array<uint8_t, 12> bytes{};
  if (length < 4 || length > UINT32_MAX || (length - 4) % 12 || !file.seek64(0) || file.read(bytes.data(), 4) != 4 ||
      std::memcmp(bytes.data(), "TDL1", 4))
    return false;
  const auto seen = scratch.first(DAY_COVERAGE_BYTES);
  std::fill(seen.begin(), seen.end(), 0);
  for (uint64_t offset = 4; offset < length; offset += 12) {
    if (!readLegacyDay(file, offset, bytes, permitted, context) || !addLegacyDayTotals(bytes, report.totals))
      return false;
    const auto day = binary_record::getU16(bytes.data());
    const auto mask = uint8_t(1u << (day % 8));
    if (!(seen[day / 8] & mask)) {
      if (!inspectLegacyDayTotals(file, length, day, permitted, context)) return false;
      seen[day / 8] |= mask;
      ++report.days;
    }
    ++report.records;
  }
  return file.fileSize64() == length && permitted(context);
}
}  // namespace unbound_course_detail
// Legacy day order is preserved. Totals are evidence, not replay correspondence.
inline bool inspectUnboundCourseDays(HalUnboundCourseReviewedFile& reviewed,
                                     const UnboundCourseMigrationRequest& request, std::span<uint8_t> scratch,
                                     UnboundCourseDayReport& output, bool (*permitted)(void*), void* context) {
  const auto failure = [] {
    LOG_ERR("COMPANION", "Unbound course day inspection refused");
    return false;
  };
  if (!permitted || !permitted(context) || scratch.size() < unbound_course_detail::DAY_COVERAGE_BYTES ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &output, sizeof(output)))
    return failure();
  const auto opened = reviewed.open(request, "days.bin");
  if (opened != UnboundReviewedFileResult::Present && opened != UnboundReviewedFileResult::Missing) return failure();
  UnboundCourseDayReport report;
  if (opened == UnboundReviewedFileResult::Missing) {
    if (!reviewed.closeReaders() || !permitted(context)) return failure();
    output = report;
    return true;
  }
  auto* file = reviewed.borrowed();
  const bool valid = file && unbound_course_detail::inspectLegacyDayFile(*file, scratch, report, permitted, context);
  const bool closed = reviewed.closeReaders();
  if (!valid || !closed || !permitted(context)) return failure();
  report.present = true;
  output = report;
  return true;
}
}  // namespace companion

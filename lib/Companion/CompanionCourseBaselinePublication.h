#pragma once

#include "CompanionCourseBaselineImportRequest.h"

namespace companion {
inline constexpr size_t COURSE_BASELINE_PUBLICATION_SIZE = 183;
enum class CourseBaselinePublicationPhase : uint8_t { Prepared = 1, Published = 2 };
// Prepared records precede archive publication; Published records follow native
// archive/copy verification. Decoding a record alone never grants authority.
struct CourseBaselinePublicationRecord {
  Identity reader{};
  CourseBaselineImportRequest request;
  CourseBaselinePublicationPhase phase = CourseBaselinePublicationPhase::Prepared;
  bool operator==(const CourseBaselinePublicationRecord&) const = default;
};
static_assert(sizeof(CourseBaselinePublicationRecord) < 256);
inline bool validCourseBaselinePublicationRecord(const CourseBaselinePublicationRecord& record) {
  return record.reader != Identity{} && validCourseBaselineImportRequest(record.request) &&
         (record.phase == CourseBaselinePublicationPhase::Prepared ||
          record.phase == CourseBaselinePublicationPhase::Published);
}
inline bool encodeCourseBaselinePublicationRecord(const CourseBaselinePublicationRecord& record,
                                                  std::span<uint8_t> output) {
  if (output.size() != COURSE_BASELINE_PUBLICATION_SIZE || !validCourseBaselinePublicationRecord(record) ||
      course_baseline_detail::overlaps(&record, sizeof(record), output.data(), output.size()))
    return false;
  static constexpr std::array<uint8_t, 8> PREFIX = {'T', 'C', 'B', 'P', 1, 0, 0, 0};
  std::copy(PREFIX.begin(), PREFIX.end(), output.begin());
  output[5] = static_cast<uint8_t>(record.phase);
  std::copy(record.reader.begin(), record.reader.end(), output.begin() + 8);
  if (!encodeCourseBaselineImportRequest(record.request, output.subspan(24, COURSE_BASELINE_IMPORT_REQUEST_SIZE)))
    return false;
  const auto crc = binary_record::crc32(output.data(), output.size() - 4);
  for (unsigned i = 0; i < 4; ++i) output[179 + i] = static_cast<uint8_t>(crc >> (8 * i));
  return true;
}
[[gnu::noinline]] inline bool decodeCourseBaselinePublicationRecord(std::span<const uint8_t> input,
                                                                    CourseBaselinePublicationRecord& output) {
  static constexpr std::array<uint8_t, 5> PREFIX = {'T', 'C', 'B', 'P', 1};
  if (input.size() != COURSE_BASELINE_PUBLICATION_SIZE ||
      course_baseline_detail::overlaps(input.data(), input.size(), &output, sizeof(output)) ||
      !std::equal(PREFIX.begin(), PREFIX.end(), input.begin()) || input[6] || input[7] ||
      (input[5] != 1 && input[5] != 2))
    return false;
  uint32_t crc = 0;
  for (unsigned i = 0; i < 4; ++i) crc |= uint32_t(input[179 + i]) << (8 * i);
  if (crc != binary_record::crc32(input.data(), input.size() - 4)) return false;
  CourseBaselinePublicationRecord parsed;
  parsed.phase = static_cast<CourseBaselinePublicationPhase>(input[5]);
  std::copy_n(input.begin() + 8, parsed.reader.size(), parsed.reader.begin());
  if (!decodeCourseBaselineImportRequest(input.subspan(24, COURSE_BASELINE_IMPORT_REQUEST_SIZE), parsed.request) ||
      !validCourseBaselinePublicationRecord(parsed))
    return false;
  output = parsed;
  return true;
}
}  // namespace companion

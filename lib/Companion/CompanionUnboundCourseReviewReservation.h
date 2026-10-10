#pragma once

#include "CompanionLegacyTintaEventCursor.h"
#include "CompanionUnboundCourseMigrationIntent.h"

namespace companion {
inline constexpr size_t UNBOUND_COURSE_REVIEW_RESERVATION_SIZE = 274;
// Retain off stack. Native code reserves this reader epoch exclusively, verifies
// the frozen stream/event counts and durably seals the record before publication.
// Decoding proves binding/integrity only, never freshness or mutation authority.
struct UnboundCourseReviewReservation {
  UnboundCourseMigrationIntent intent;
  uint64_t epoch = 0;
  uint32_t records = 0, events = 0;
  EventIdentity first() const { return {intent.reader, epoch, 1}; }
  bool operator==(const UnboundCourseReviewReservation&) const = default;
};
namespace unbound_course_detail {
inline constexpr std::array<uint8_t, 8> REVIEW_RESERVATION_PREFIX = {'T', 'C', 'R', 'R', 1, 0, 0, 0};
inline bool validReviewReservationCounts(uint32_t records, uint32_t events) {
  return records <= LegacyTintaJournalDecoder::MAX_BYTES / LegacyTintaJournalDecoder::RECORD_SIZE &&
         events >= records && uint64_t(events) <= uint64_t(records) * 2;
}
}  // namespace unbound_course_detail
inline bool validUnboundCourseReviewReservation(const UnboundCourseReviewReservation& value) {
  return validUnboundCourseMigrationIntent(value.intent) &&
         value.intent.phase == UnboundCourseMigrationPhase::Prepared && value.epoch &&
         unbound_course_detail::validReviewReservationCounts(value.records, value.events);
}
inline bool encodeUnboundCourseReviewReservation(const UnboundCourseReviewReservation& value,
                                                 std::span<uint8_t> output) {
  if (output.size() != UNBOUND_COURSE_REVIEW_RESERVATION_SIZE || !validUnboundCourseReviewReservation(value) ||
      course_baseline_detail::overlaps(&value, sizeof(value), output.data(), output.size()))
    return false;
  std::copy(unbound_course_detail::REVIEW_RESERVATION_PREFIX.begin(),
            unbound_course_detail::REVIEW_RESERVATION_PREFIX.end(), output.begin());
  if (!encodeUnboundCourseMigrationIntent(value.intent, output.subspan(8, UNBOUND_COURSE_MIGRATION_INTENT_SIZE)))
    return false;
  course_review_detail::number(output, 254, value.epoch, 8);
  course_review_detail::number(output, 262, value.records, 4);
  course_review_detail::number(output, 266, value.events, 4);
  course_review_detail::number(output, 270, binary_record::crc32(output.data(), 270), 4);
  return true;
}
inline bool validEncodedUnboundCourseReviewReservation(std::span<const uint8_t> input) {
  if (input.size() != UNBOUND_COURSE_REVIEW_RESERVATION_SIZE ||
      !std::equal(unbound_course_detail::REVIEW_RESERVATION_PREFIX.begin(),
                  unbound_course_detail::REVIEW_RESERVATION_PREFIX.end(), input.begin()) ||
      course_review_detail::number(input, 270, 4) != binary_record::crc32(input.data(), 270) ||
      !validEncodedUnboundCourseMigrationIntent(input.subspan(8, UNBOUND_COURSE_MIGRATION_INTENT_SIZE)) ||
      input[13] != static_cast<uint8_t>(UnboundCourseMigrationPhase::Prepared))
    return false;
  const auto epoch = course_review_detail::number(input, 254, 8);
  const auto records = static_cast<uint32_t>(course_review_detail::number(input, 262, 4));
  const auto events = static_cast<uint32_t>(course_review_detail::number(input, 266, 4));
  return epoch && unbound_course_detail::validReviewReservationCounts(records, events);
}
// Input is immutable for both validation and extraction; output is preserved on failure.
inline bool decodeUnboundCourseReviewReservation(std::span<const uint8_t> input,
                                                 UnboundCourseReviewReservation& output) {
  if (course_baseline_detail::overlaps(input.data(), input.size(), &output, sizeof(output)) ||
      !validEncodedUnboundCourseReviewReservation(input))
    return false;
  decodeUnboundCourseMigrationIntent(input.subspan(8, UNBOUND_COURSE_MIGRATION_INTENT_SIZE), output.intent);
  output.epoch = course_review_detail::number(input, 254, 8);
  output.records = static_cast<uint32_t>(course_review_detail::number(input, 262, 4));
  output.events = static_cast<uint32_t>(course_review_detail::number(input, 266, 4));
  return true;
}
}  // namespace companion

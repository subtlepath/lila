#pragma once

#include "CompanionUnboundCourseReviewReservation.h"

namespace companion {
inline constexpr size_t UNBOUND_COURSE_STAR_RESERVATION_SIZE = 330;
inline constexpr uint32_t UNBOUND_COURSE_STAR_CAPACITY = 96;
// Native owner must exclusively reserve/seal the later epoch and verify the
// frozen plan before publication. Integrity is not epoch ownership or authority.
struct UnboundCourseStarReservation {
  UnboundCourseReviewReservation reviews;
  uint64_t epoch = 0;
  uint32_t events = 0;
  // SHA-256 of the ordered concatenation of canonical 23-byte Star bodies.
  Digest planHash{};
  EventIdentity first() const { return {reviews.intent.reader, epoch, 1}; }
  EventIdentity reviewTail() const { return {reviews.intent.reader, reviews.epoch, reviews.events}; }
  bool operator==(const UnboundCourseStarReservation&) const = default;
};
namespace unbound_course_detail {
inline constexpr std::array<uint8_t, 8> STAR_RESERVATION_PREFIX = {'T', 'C', 'S', 'R', 1, 0, 0, 0};
inline bool validStarReservationCounts(uint32_t records, uint32_t events) {
  return uint64_t(events) <= uint64_t(records) + UNBOUND_COURSE_STAR_CAPACITY;
}
}  // namespace unbound_course_detail
inline bool validUnboundCourseStarReservation(const UnboundCourseStarReservation& value) {
  return validUnboundCourseReviewReservation(value.reviews) && value.epoch > value.reviews.epoch &&
         unbound_course_detail::validStarReservationCounts(value.reviews.records, value.events) &&
         tinta_body_detail::nonzero(value.planHash);
}
inline bool encodeUnboundCourseStarReservation(const UnboundCourseStarReservation& value, std::span<uint8_t> output) {
  if (output.size() != UNBOUND_COURSE_STAR_RESERVATION_SIZE || !validUnboundCourseStarReservation(value) ||
      course_baseline_detail::overlaps(&value, sizeof(value), output.data(), output.size()))
    return false;
  std::copy(unbound_course_detail::STAR_RESERVATION_PREFIX.begin(),
            unbound_course_detail::STAR_RESERVATION_PREFIX.end(), output.begin());
  if (!encodeUnboundCourseReviewReservation(value.reviews, output.subspan(8, UNBOUND_COURSE_REVIEW_RESERVATION_SIZE)))
    return false;
  course_review_detail::number(output, 282, value.epoch, 8);
  course_review_detail::number(output, 290, value.events, 4);
  std::copy(value.planHash.begin(), value.planHash.end(), output.begin() + 294);
  course_review_detail::number(output, 326, binary_record::crc32(output.data(), 326), 4);
  return true;
}
// Immutable input; validate the full nested record before assigning any output.
inline bool decodeUnboundCourseStarReservation(std::span<const uint8_t> input, UnboundCourseStarReservation& output) {
  if (input.size() != UNBOUND_COURSE_STAR_RESERVATION_SIZE ||
      course_baseline_detail::overlaps(input.data(), input.size(), &output, sizeof(output)) ||
      !std::equal(unbound_course_detail::STAR_RESERVATION_PREFIX.begin(),
                  unbound_course_detail::STAR_RESERVATION_PREFIX.end(), input.begin()) ||
      course_review_detail::number(input, 326, 4) != binary_record::crc32(input.data(), 326) ||
      !validEncodedUnboundCourseReviewReservation(input.subspan(8, UNBOUND_COURSE_REVIEW_RESERVATION_SIZE)) ||
      course_review_detail::number(input, 282, 8) <= course_review_detail::number(input, 262, 8) ||
      !unbound_course_detail::validStarReservationCounts(
          static_cast<uint32_t>(course_review_detail::number(input, 270, 4)),
          static_cast<uint32_t>(course_review_detail::number(input, 290, 4))) ||
      !tinta_body_detail::nonzero(input.subspan(294, 32)))
    return false;
  decodeUnboundCourseReviewReservation(input.subspan(8, UNBOUND_COURSE_REVIEW_RESERVATION_SIZE), output.reviews);
  output.epoch = course_review_detail::number(input, 282, 8);
  output.events = static_cast<uint32_t>(course_review_detail::number(input, 290, 4));
  std::copy_n(input.begin() + 294, 32, output.planHash.begin());
  return true;
}
}  // namespace companion

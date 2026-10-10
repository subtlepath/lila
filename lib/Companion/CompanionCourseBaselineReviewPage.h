#pragma once

#include "CompanionCourseBaselineReview.h"
#include "CompanionFrame.h"

namespace companion {
inline constexpr size_t COURSE_BASELINE_REVIEW_PAGE_OVERHEAD = 48;
inline constexpr size_t COURSE_BASELINE_REVIEW_PAGE_MAX_BYTES =
    MAX_CONTROL_PAYLOAD - COURSE_BASELINE_REVIEW_PAGE_OVERHEAD;
inline constexpr size_t COURSE_BASELINE_REVIEW_PAGE_REQUEST_SIZE = 74;
struct CourseBaselineReviewPageRequest {
  Identity generation{}, course{};
  Digest hash{};
  uint16_t offset = 0, limit = 97;
  bool operator==(const CourseBaselineReviewPageRequest&) const = default;
};
static_assert(sizeof(CourseBaselineReviewPageRequest) < 256);
struct CourseBaselineReviewPageView {
  uint16_t total = 0, offset = 0;
  Digest hash{};
  std::span<const uint8_t> bytes;
};
namespace course_review_page_detail {
inline constexpr std::array<uint8_t, 6> PREFIX = {'T', 'C', 'B', 'P', 1, 0};
inline constexpr std::array<uint8_t, 6> REQUEST_PREFIX = {'T', 'C', 'B', 'Q', 1, 0};
inline bool valid(const CourseBaselineReviewPageRequest& request) {
  return course_review_detail::nonzero(request.generation) && course_review_detail::nonzero(request.course) &&
         request.offset < COURSE_BASELINE_REVIEW_MAX_SIZE && request.limit &&
         request.limit <= COURSE_BASELINE_REVIEW_PAGE_MAX_BYTES &&
         (course_review_detail::nonzero(request.hash) || !request.offset);
}
inline bool valid(size_t total, size_t offset, size_t count) {
  return total >= 64 + 8 * COURSE_BASELINE_REVIEW_ENTRY_SIZE && total <= COURSE_BASELINE_REVIEW_MAX_SIZE &&
         (total - 64) % COURSE_BASELINE_REVIEW_ENTRY_SIZE == 0 && offset < total && count &&
         count <= COURSE_BASELINE_REVIEW_PAGE_MAX_BYTES && count <= total - offset;
}
}  // namespace course_review_page_detail
// Zero hash requests an initial capture; a nonzero hash selects an immutable roster.
inline bool encodeCourseBaselineReviewPageRequest(const CourseBaselineReviewPageRequest& request,
                                                  std::span<uint8_t> output) {
  if (output.size() != COURSE_BASELINE_REVIEW_PAGE_REQUEST_SIZE || !course_review_page_detail::valid(request) ||
      course_baseline_detail::overlaps(&request, sizeof(request), output.data(), output.size()))
    return false;
  std::copy(course_review_page_detail::REQUEST_PREFIX.begin(), course_review_page_detail::REQUEST_PREFIX.end(),
            output.begin());
  std::copy(request.generation.begin(), request.generation.end(), output.begin() + 6);
  std::copy(request.course.begin(), request.course.end(), output.begin() + 22);
  std::copy(request.hash.begin(), request.hash.end(), output.begin() + 38);
  course_review_detail::number(output, 70, request.offset, 2);
  course_review_detail::number(output, 72, request.limit, 2);
  return true;
}
inline bool decodeCourseBaselineReviewPageRequest(std::span<const uint8_t> input,
                                                  CourseBaselineReviewPageRequest& output) {
  if (input.size() != COURSE_BASELINE_REVIEW_PAGE_REQUEST_SIZE ||
      course_baseline_detail::overlaps(input.data(), input.size(), &output, sizeof(output)) ||
      !std::equal(course_review_page_detail::REQUEST_PREFIX.begin(), course_review_page_detail::REQUEST_PREFIX.end(),
                  input.begin()))
    return false;
  CourseBaselineReviewPageRequest parsed;
  std::copy_n(input.begin() + 6, parsed.generation.size(), parsed.generation.begin());
  std::copy_n(input.begin() + 22, parsed.course.size(), parsed.course.begin());
  std::copy_n(input.begin() + 38, parsed.hash.size(), parsed.hash.begin());
  parsed.offset = course_review_detail::number(input, 70, 2);
  parsed.limit = course_review_detail::number(input, 72, 2);
  if (!course_review_page_detail::valid(parsed)) return false;
  output = parsed;
  return true;
}
// The owner supplies the verified whole-review SHA-256. Paging grants no consent.
// Input must remain immutable until all borrowed pages have been consumed.
inline size_t encodeCourseBaselineReviewPage(std::span<const uint8_t> review, const Digest& verifiedHash, size_t offset,
                                             size_t limit, std::span<uint8_t> output) {
  CourseBaselineReviewView view;
  if (!view.decode(review, true) || !course_review_detail::nonzero(verifiedHash) || !limit ||
      limit > COURSE_BASELINE_REVIEW_PAGE_MAX_BYTES || offset >= review.size() ||
      course_baseline_detail::overlaps(review.data(), review.size(), output.data(), output.size()) ||
      course_baseline_detail::overlaps(&verifiedHash, sizeof(verifiedHash), output.data(), output.size()))
    return 0;
  const auto count = std::min(limit, review.size() - offset);
  const auto length = COURSE_BASELINE_REVIEW_PAGE_OVERHEAD + count;
  if (output.size() < length) return 0;
  auto bytes = output.first(length);
  std::copy(course_review_page_detail::PREFIX.begin(), course_review_page_detail::PREFIX.end(), bytes.begin());
  course_review_detail::number(bytes, 6, review.size(), 2);
  course_review_detail::number(bytes, 8, offset, 2);
  course_review_detail::number(bytes, 10, count, 2);
  std::copy(verifiedHash.begin(), verifiedHash.end(), bytes.begin() + 12);
  std::copy_n(review.begin() + offset, count, bytes.begin() + 44);
  course_review_detail::number(bytes, length - 4, binary_record::crc32(bytes.data(), length - 4), 4);
  return length;
}
// A page is framing evidence only. Reassemble and verify the whole review/hash,
// reader, storage generation and course before presenting import approval.
inline bool decodeCourseBaselineReviewPage(std::span<const uint8_t> input, CourseBaselineReviewPageView& output) {
  if (input.size() <= COURSE_BASELINE_REVIEW_PAGE_OVERHEAD || input.size() > MAX_CONTROL_PAYLOAD ||
      course_baseline_detail::overlaps(input.data(), input.size(), &output, sizeof(output)) ||
      !std::equal(course_review_page_detail::PREFIX.begin(), course_review_page_detail::PREFIX.end(), input.begin()))
    return false;
  const auto total = course_review_detail::number(input, 6, 2);
  const auto offset = course_review_detail::number(input, 8, 2);
  const auto count = course_review_detail::number(input, 10, 2);
  if (!course_review_page_detail::valid(total, offset, count) ||
      input.size() != COURSE_BASELINE_REVIEW_PAGE_OVERHEAD + count ||
      !course_review_detail::nonzero(input.subspan(12, 32)) ||
      course_review_detail::number(input, input.size() - 4, 4) != binary_record::crc32(input.data(), input.size() - 4))
    return false;
  CourseBaselineReviewPageView decoded;
  decoded.total = total;
  decoded.offset = offset;
  std::copy_n(input.begin() + 12, decoded.hash.size(), decoded.hash.begin());
  decoded.bytes = input.subspan(44, count);
  output = decoded;
  return true;
}
}  // namespace companion

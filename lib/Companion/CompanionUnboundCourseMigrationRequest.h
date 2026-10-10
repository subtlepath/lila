#pragma once

#include "CompanionCourseBaselineReview.h"

namespace companion {
inline constexpr size_t UNBOUND_COURSE_MIGRATION_REQUEST_SIZE = COURSE_BASELINE_IMPORT_REQUEST_SIZE;
// Explicit assignment of reviewed global learner state to the original pack's course.
// Pack validation, verified backups and recoverable isolation are separate obligations.
struct UnboundCourseMigrationRequest {
  CourseBaselineImportRequest original;
  bool operator==(const UnboundCourseMigrationRequest&) const = default;
};
static_assert(sizeof(UnboundCourseMigrationRequest) < 256);
namespace unbound_course_detail {
inline constexpr std::array<uint8_t, 8> PREFIX = {'T', 'C', 'U', 'M', 1, 1, 0, 0};
}
inline bool encodeUnboundCourseMigrationRequest(const UnboundCourseMigrationRequest& request,
                                                std::span<uint8_t> output) {
  if (!encodeCourseBaselineImportRequest(request.original, output)) return false;
  std::copy(unbound_course_detail::PREFIX.begin(), unbound_course_detail::PREFIX.end(), output.begin());
  course_review_detail::number(output, 151, binary_record::crc32(output.data(), 151), 4);
  return true;
}
[[gnu::noinline]] inline bool decodeUnboundCourseMigrationRequest(std::span<const uint8_t> input,
                                                                 UnboundCourseMigrationRequest& output) {
  if (input.size() != UNBOUND_COURSE_MIGRATION_REQUEST_SIZE ||
      course_baseline_detail::overlaps(input.data(), input.size(), &output, sizeof(output)) ||
      !std::equal(unbound_course_detail::PREFIX.begin(), unbound_course_detail::PREFIX.end(), input.begin()) ||
      course_review_detail::number(input, 151, 4) != binary_record::crc32(input.data(), 151))
    return false;
  UnboundCourseMigrationRequest parsed;
  std::copy_n(input.begin() + 8, 16, parsed.original.generation.begin());
  std::copy_n(input.begin() + 24, 16, parsed.original.owner.begin());
  std::copy_n(input.begin() + 40, 16, parsed.original.transaction.begin());
  if (!decodeRecord(input.subspan(56, CONTENT_MANIFEST_SIZE), parsed.original.manifest)) return false;
  std::copy_n(input.begin() + 119, 32, parsed.original.reviewHash.begin());
  if (!validCourseBaselineImportRequest(parsed.original)) return false;
  output = parsed;
  return true;
}
// Caller authenticates native identities and hashes the immutable review bytes.
inline bool matchesUnboundCourseMigrationRequest(const UnboundCourseMigrationRequest& request,
                                                 std::span<const uint8_t> review, const Digest& verifiedHash,
                                                 const Identity& reader, const Identity& generation,
                                                 const Identity& owner, const TransferDeclaration& transfer) {
  CourseBaselineReviewView view;
  return view.decode(review, true) && !view.isolated() &&
         std::equal(view.reader().begin(), view.reader().end(), reader.begin()) &&
         std::equal(view.generation().begin(), view.generation().end(), generation.begin()) &&
         std::equal(view.course().begin(), view.course().end(), request.original.manifest.logicalIdentity.begin()) &&
         matchesCourseBaselineImportRequest(request.original, generation, owner, verifiedHash, transfer);
}
}  // namespace companion

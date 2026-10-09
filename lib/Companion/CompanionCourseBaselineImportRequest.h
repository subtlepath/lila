#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionCourseBinding.h"
#include "CompanionTransferDeclaration.h"

namespace companion {
inline constexpr size_t COURSE_BASELINE_IMPORT_REQUEST_SIZE = 155;
// Explicit confirmation that this is the original pack for the reviewed state.
// reviewHash binds a separately verified, frozen learner-state review; it is not
// evidence that an arbitrary replacement pack preserves existing item meaning.
struct CourseBaselineImportRequest {
  Identity generation{}, owner{}, transaction{};
  ContentManifest manifest{};
  Digest reviewHash{};
  bool operator==(const CourseBaselineImportRequest&) const = default;
};
static_assert(sizeof(CourseBaselineImportRequest) < 256);
inline bool validCourseBaselineImportRequest(const CourseBaselineImportRequest& request) {
  const auto nonzero = [](const auto& bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte != 0; });
  };
  return nonzero(request.generation) && nonzero(request.owner) && nonzero(request.transaction) &&
         nonzero(request.reviewHash) && validCourseBinding(request.manifest) && request.manifest.formatVersion == 1 &&
         request.manifest.length <= UINT32_MAX && nonzero(request.manifest.contentHash);
}
inline bool matchesCourseBaselineImportRequest(const CourseBaselineImportRequest& request, const Identity& generation,
                                               const Identity& owner, const Digest& reviewed,
                                               const TransferDeclaration& transfer) {
  return validCourseBaselineImportRequest(request) && validTransferDeclaration(transfer) &&
         request.generation == generation && request.owner == owner && request.reviewHash == reviewed &&
         request.generation == transfer.state.storageGeneration && request.owner == transfer.state.owner &&
         request.transaction == transfer.state.transaction && request.manifest == transfer.manifest;
}
namespace course_baseline_detail {
inline bool overlaps(const void* first, size_t firstSize, const void* second, size_t secondSize) {
  const auto a = reinterpret_cast<uintptr_t>(first), b = reinterpret_cast<uintptr_t>(second);
  return a <= b ? firstSize > b - a : secondSize > a - b;
}
inline constexpr std::array<uint8_t, 8> PREFIX = {'T', 'C', 'B', 'I', 1, 1, 0, 0};
}  // namespace course_baseline_detail
inline bool encodeCourseBaselineImportRequest(const CourseBaselineImportRequest& request, std::span<uint8_t> output) {
  if (output.size() != COURSE_BASELINE_IMPORT_REQUEST_SIZE || !validCourseBaselineImportRequest(request) ||
      course_baseline_detail::overlaps(&request, sizeof(request), output.data(), output.size()))
    return false;
  std::copy(course_baseline_detail::PREFIX.begin(), course_baseline_detail::PREFIX.end(), output.begin());
  std::copy(request.generation.begin(), request.generation.end(), output.begin() + 8);
  std::copy(request.owner.begin(), request.owner.end(), output.begin() + 24);
  std::copy(request.transaction.begin(), request.transaction.end(), output.begin() + 40);
  if (encodeRecord(request.manifest, output.subspan(56, CONTENT_MANIFEST_SIZE)) != CONTENT_MANIFEST_SIZE) return false;
  std::copy(request.reviewHash.begin(), request.reviewHash.end(), output.begin() + 119);
  const auto crc = binary_record::crc32(output.data(), 151);
  for (unsigned at = 0; at < 4; ++at) output[151 + at] = static_cast<uint8_t>(crc >> (8 * at));
  return true;
}
[[gnu::noinline]] inline bool decodeCourseBaselineImportRequest(std::span<const uint8_t> input,
                                                                CourseBaselineImportRequest& output) {
  if (input.size() != COURSE_BASELINE_IMPORT_REQUEST_SIZE ||
      course_baseline_detail::overlaps(input.data(), input.size(), &output, sizeof(output)) ||
      !std::equal(course_baseline_detail::PREFIX.begin(), course_baseline_detail::PREFIX.end(), input.begin()))
    return false;
  uint32_t crc = 0;
  for (unsigned at = 0; at < 4; ++at) crc |= uint32_t(input[151 + at]) << (8 * at);
  if (crc != binary_record::crc32(input.data(), 151)) return false;
  CourseBaselineImportRequest parsed;
  std::copy_n(input.begin() + 8, 16, parsed.generation.begin());
  std::copy_n(input.begin() + 24, 16, parsed.owner.begin());
  std::copy_n(input.begin() + 40, 16, parsed.transaction.begin());
  if (!decodeRecord(input.subspan(56, CONTENT_MANIFEST_SIZE), parsed.manifest)) return false;
  std::copy_n(input.begin() + 119, 32, parsed.reviewHash.begin());
  if (!validCourseBaselineImportRequest(parsed)) return false;
  output = parsed;
  return true;
}
}  // namespace companion

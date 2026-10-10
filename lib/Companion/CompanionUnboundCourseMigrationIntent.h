#pragma once

#include "CompanionUnboundCourseMigrationRequest.h"

namespace companion {
inline constexpr size_t UNBOUND_COURSE_MIGRATION_INTENT_SIZE = 246;
enum class UnboundCourseMigrationPhase : uint8_t { Prepared = 1, Bound = 2, Isolated = 3 };
// Prepared precedes namespace mutation; subsequent phases require native verification.
// Decoding retained intent is evidence, never permission to move learner files.
struct UnboundCourseMigrationIntent {
  Identity reader{};
  UnboundCourseMigrationRequest request;
  ContentManifest activePack;
  UnboundCourseMigrationPhase phase = UnboundCourseMigrationPhase::Prepared;
  bool operator==(const UnboundCourseMigrationIntent&) const = default;
};
namespace unbound_course_detail {
inline constexpr std::array<uint8_t, 5> INTENT_PREFIX = {'T', 'C', 'U', 'I', 1};
inline bool validActivePack(const ContentManifest& pack) {
  return validCourseBinding(pack) && pack.formatVersion == 1 && pack.length <= UINT32_MAX &&
         pack.contentHash != Digest{};
}
[[gnu::noinline]] inline bool validIntentRequest(std::span<const uint8_t> bytes) {
  UnboundCourseMigrationRequest request;
  return decodeUnboundCourseMigrationRequest(bytes, request);
}
[[gnu::noinline]] inline bool validIntentPack(std::span<const uint8_t> bytes) {
  ContentManifest pack;
  return decodeRecord(bytes, pack) && validActivePack(pack);
}
}  // namespace unbound_course_detail
inline bool validUnboundCourseMigrationIntent(const UnboundCourseMigrationIntent& intent) {
  return intent.reader != Identity{} && validCourseBaselineImportRequest(intent.request.original) &&
         unbound_course_detail::validActivePack(intent.activePack) &&
         intent.activePack.logicalIdentity == intent.request.original.manifest.logicalIdentity &&
         (intent.phase == UnboundCourseMigrationPhase::Prepared || intent.phase == UnboundCourseMigrationPhase::Bound ||
          intent.phase == UnboundCourseMigrationPhase::Isolated);
}
inline bool encodeUnboundCourseMigrationIntent(const UnboundCourseMigrationIntent& intent, std::span<uint8_t> output) {
  if (output.size() != UNBOUND_COURSE_MIGRATION_INTENT_SIZE || !validUnboundCourseMigrationIntent(intent) ||
      course_baseline_detail::overlaps(&intent, sizeof(intent), output.data(), output.size()))
    return false;
  std::copy(unbound_course_detail::INTENT_PREFIX.begin(), unbound_course_detail::INTENT_PREFIX.end(), output.begin());
  output[5] = static_cast<uint8_t>(intent.phase);
  output[6] = output[7] = 0;
  std::copy(intent.reader.begin(), intent.reader.end(), output.begin() + 8);
  if (!encodeUnboundCourseMigrationRequest(intent.request, output.subspan(24, UNBOUND_COURSE_MIGRATION_REQUEST_SIZE)) ||
      encodeRecord(intent.activePack, output.subspan(179, CONTENT_MANIFEST_SIZE)) != CONTENT_MANIFEST_SIZE)
    return false;
  course_review_detail::number(output, 242, binary_record::crc32(output.data(), 242), 4);
  return true;
}
inline bool decodeUnboundCourseMigrationIntent(std::span<const uint8_t> input, UnboundCourseMigrationIntent& output) {
  if (input.size() != UNBOUND_COURSE_MIGRATION_INTENT_SIZE ||
      course_baseline_detail::overlaps(input.data(), input.size(), &output, sizeof(output)) ||
      !std::equal(unbound_course_detail::INTENT_PREFIX.begin(), unbound_course_detail::INTENT_PREFIX.end(),
                  input.begin()) ||
      input[5] < 1 || input[5] > 3 || input[6] || input[7] || !course_review_detail::nonzero(input.subspan(8, 16)) ||
      course_review_detail::number(input, 242, 4) != binary_record::crc32(input.data(), 242) ||
      !unbound_course_detail::validIntentRequest(input.subspan(24, UNBOUND_COURSE_MIGRATION_REQUEST_SIZE)) ||
      !unbound_course_detail::validIntentPack(input.subspan(179, CONTENT_MANIFEST_SIZE)) ||
      !std::equal(input.begin() + 127, input.begin() + 143, input.begin() + 226))
    return false;
  // Validate immutable input first to retain output on failure without a large stack copy.
  decodeUnboundCourseMigrationRequest(input.subspan(24, UNBOUND_COURSE_MIGRATION_REQUEST_SIZE), output.request);
  decodeRecord(input.subspan(179, CONTENT_MANIFEST_SIZE), output.activePack);
  std::copy_n(input.begin() + 8, output.reader.size(), output.reader.begin());
  output.phase = static_cast<UnboundCourseMigrationPhase>(input[5]);
  return true;
}
}  // namespace companion

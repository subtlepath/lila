#pragma once

#include "CompanionCourseRemovalPlan.h"
#include "CompanionCourseRemovalProof.h"

namespace companion {
// Address selection only. Caller hashes the sealed plan before passing its digest,
// then verifies cached pack bytes and scope isolation while excluding writers.
inline bool historicalCourseRemovalCachePath(const ContentRemovalRecord& completed, const CourseRemovalPlan& plan,
                                             const Digest& verifiedPlanHash, const Identity& generation,
                                             const Identity& course, std::span<char> output) {
  if (!validContentRemovalRecord(completed) || completed.phase != ContentRemovalPhase::Retired ||
      !validCourseRemovalPlan(plan) || plan.request != completed.request || completed.planHash != verifiedPlanHash ||
      completed.request.generation != generation || completed.request.manifest.logicalIdentity != course ||
      completed.request.manifest.formatVersion != 1 || output.size() < COURSE_REMOVAL_CACHE_PATH_CAPACITY)
    return false;
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  constexpr size_t PREFIX_SIZE = sizeof(COURSE_REMOVAL_CACHE_PREFIX) - 1;
  std::copy_n(COURSE_REMOVAL_CACHE_PREFIX, PREFIX_SIZE, output.begin());
  size_t at = PREFIX_SIZE;
  for (const auto byte : verifiedPlanHash) {
    output[at++] = HEX_DIGITS[byte >> 4];
    output[at++] = HEX_DIGITS[byte & 15];
  }
  output[at] = 0;
  return true;
}
}  // namespace companion

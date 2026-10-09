#pragma once

#include "CompanionContentRemovalJournal.h"
#include "CompanionCourseBinding.h"

namespace companion {
inline constexpr char COURSE_REMOVAL_PROOF_PATH[] = "/.crosspoint/companion/course-removed";
inline constexpr char COURSE_REMOVAL_PROOF_STAGE[] = "/.crosspoint/companion/course-removed.tmp";
inline constexpr char COURSE_REMOVAL_CACHE_PREFIX[] = "/.crosspoint/companion/course-removed-";
inline constexpr size_t COURSE_REMOVAL_CACHE_PATH_CAPACITY = sizeof(COURSE_REMOVAL_CACHE_PREFIX) + 64;

// Publication retains the quarantined checkpoint. A separate Retired receipt
// authorizes use of the cached baseline after the active journal is released.
// The caller verifies the sealed plan and cached pack length/SHA independently.
inline bool validCourseRemovalProof(const ContentRemovalRecord& proof) {
  return validContentRemovalRecord(proof) && proof.request.manifest.kind == ContentKind::Course &&
         proof.phase == ContentRemovalPhase::Quarantined;
}
inline bool matchesCourseRemovalProof(const ContentRemovalRecord& proof, const ContentRemovalRecord& checkpoint) {
  return validCourseRemovalProof(proof) && validContentRemovalRecord(checkpoint) &&
         checkpoint.phase >= ContentRemovalPhase::Quarantined && checkpoint.request == proof.request &&
         checkpoint.planHash == proof.planHash;
}
inline bool completedCourseRemovalProof(const ContentRemovalRecord& proof, const ContentRemovalRecord& completed,
                                        const ContentManifest& binding, const Identity& generation) {
  return matchesCourseRemovalProof(proof, completed) && completed.phase == ContentRemovalPhase::Retired &&
         binding == proof.request.manifest && generation == proof.request.generation;
}
inline bool courseRemovalCachePath(const ContentRemovalRecord& proof, std::span<char> output) {
  if (!validCourseRemovalProof(proof) || output.size() < COURSE_REMOVAL_CACHE_PATH_CAPACITY) return false;
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  constexpr size_t PREFIX_SIZE = sizeof(COURSE_REMOVAL_CACHE_PREFIX) - 1;
  std::copy_n(COURSE_REMOVAL_CACHE_PREFIX, PREFIX_SIZE, output.begin());
  size_t at = PREFIX_SIZE;
  for (const auto byte : proof.planHash) {
    output[at++] = HEX_DIGITS[byte >> 4];
    output[at++] = HEX_DIGITS[byte & 15];
  }
  output[at] = 0;
  return true;
}
}  // namespace companion

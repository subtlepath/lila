#pragma once

#include "CompanionCourseBaselineReviewPage.h"
#include "HalCompanionHeapAdmission.h"
#include "HalCourseBaselineReviewStore.h"

namespace companion {
// Permission proves authentication and exclusive ownership of the full workspace.
// Store comparison scratch must be outside the review region. No consent is saved.
inline size_t readHalCourseBaselineReviewPage(HalCourseBaselineReviewStore& store,
                                              HalCourseBaselineReviewStore::Permission permitted, void* context,
                                              const Digest& expected, const Identity& reader,
                                              const Identity& generation, const Identity& course, size_t offset,
                                              size_t limit, std::span<uint8_t> scratch, std::span<uint8_t> response) {
  if (!permitted || !permitted(context) || !Storage.ready() || !admitCompanionHeap() ||
      scratch.size() < COURSE_BASELINE_REVIEW_MAX_SIZE ||
      course_baseline_detail::overlaps(scratch.data(), COURSE_BASELINE_REVIEW_MAX_SIZE, response.data(),
                                       response.size()) ||
      course_baseline_detail::overlaps(response.data(), response.size(), &store, sizeof(store))) {
    LOG_ERR("COMPANION", "Baseline review page workspace or permission refused");
    return 0;
  }
  const Digest hash = expected;
  if (store.open(hash, reader, generation, course, scratch.first(COURSE_BASELINE_REVIEW_MAX_SIZE)) !=
      CourseBaselineReviewStoreResult::Ok) {
    store.closeReaders();
    LOG_ERR("COMPANION", "Baseline review page source refused");
    return 0;
  }
  const auto length = 64 + course_review_detail::number(scratch, 56, 2) * COURSE_BASELINE_REVIEW_ENTRY_SIZE;
  if (!store.closeReaders() || !permitted(context) || !Storage.ready() || !admitCompanionHeap()) {
    LOG_ERR("COMPANION", "Baseline review page close or permission refused");
    return 0;
  }
  const auto encoded = encodeCourseBaselineReviewPage(scratch.first(length), hash, offset, limit, response);
  if (!encoded || !permitted(context) || !Storage.ready() || !admitCompanionHeap()) {
    LOG_ERR("COMPANION", "Baseline review page encoding or permission refused");
    return 0;
  }
  return encoded;
}
}  // namespace companion

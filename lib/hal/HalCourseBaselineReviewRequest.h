#pragma once

#include <Memory.h>

#include "HalCourseBaselineReviewBackup.h"
#include "HalCourseBaselineReviewCapture.h"
#include "HalCourseBaselineReviewPage.h"

namespace companion {
// Caller has finished recovery and owns an authenticated full-workspace lease.
// Retained store uses the same permission/context and disjoint comparison scratch.
// Request and native identities remain outside the workspace for the operation.
// Capture seals a roster only; learner files and import consent remain untouched.
inline size_t handleHalCourseBaselineReviewRequest(HalCourseBaselineReviewStore& store, const Identity& nativeReader,
                                                   const Identity& generation,
                                                   const CourseBaselineReviewPageRequest& selected,
                                                   HalCourseBaselineReviewStore::Permission permitted, void* context,
                                                   std::span<uint8_t> scratch, std::span<uint8_t> response) {
  Digest hash = selected.hash;
  const auto start = reinterpret_cast<uintptr_t>(scratch.data());
  const auto replyStart = reinterpret_cast<uintptr_t>(response.data());
  if (!permitted || !permitted(context) || !Storage.ready() || !admitCompanionHeap() ||
      !course_review_page_detail::valid(selected) || nativeReader == Identity{} || selected.generation != generation ||
      scratch.size() != SESSION_WORKSPACE_SIZE ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &selected, sizeof(selected)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &nativeReader, sizeof(nativeReader)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &generation, sizeof(generation)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &store, sizeof(store)) ||
      response.size() < COURSE_BASELINE_REVIEW_PAGE_OVERHEAD + selected.limit || response.size() > scratch.size() ||
      replyStart < start || replyStart - start > scratch.size() - response.size() ||
      replyStart - start < COURSE_BASELINE_REVIEW_MAX_SIZE) {
    LOG_ERR("COMPANION", "Baseline review request context or workspace refused");
    return 0;
  }
  if (!store.closeReaders()) {
    LOG_ERR("COMPANION", "Baseline review store close refused");
    return 0;
  }
  size_t captured = 0;
  bool unbound = false;
  if (!course_review_detail::nonzero(hash)) {
    if (!admitCompanionHeap(sizeof(HalCourseBaselineReviewCapture), sizeof(HalCourseBaselineReviewCapture))) return 0;
    auto capture = makeUniqueNoThrow<HalCourseBaselineReviewCapture>(scratch, permitted, context);
    if (!capture) {
      LOG_ERR("COMPANION", "OOM: baseline review capture");
      return 0;
    }
    if (capture->capture(nativeReader, selected.generation, selected.course) != CourseBaselineReviewResult::Ok) {
      LOG_ERR("COMPANION", "Baseline review capture refused");
      return 0;
    }
    const auto* verified = capture->hash();
    const auto bytes = capture->bytes();
    if (!verified || bytes.empty()) {
      LOG_ERR("COMPANION", "Baseline review capture loan refused");
      return 0;
    }
    hash = *verified;
    captured = bytes.size();
    unbound = bytes[6] == 1;
    if (!capture->closeReaders() || !permitted(context) || !admitCompanionHeap()) {
      LOG_ERR("COMPANION", "Baseline review capture close or permission refused");
      return 0;
    }
  }
  if (!permitted(context) || !Storage.ready() || !admitCompanionHeap()) {
    LOG_ERR("COMPANION", "Baseline review sealing permission refused");
    return 0;
  }
  if (captured && store.publish(scratch.first(captured), hash) != CourseBaselineReviewStoreResult::Ok) {
    LOG_ERR("COMPANION", "Baseline review sealing refused");
    return 0;
  }
  if (unbound) {
    if (!store.closeReaders() ||
        !admitCompanionHeap(sizeof(HalCourseBaselineReviewBackup), sizeof(HalCourseBaselineReviewBackup))) {
      LOG_ERR("COMPANION", "Legacy review backup admission refused");
      return 0;
    }
    // Fixed paths, hashes and file handles exceed the stack budget; reuse the session workspace.
    auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, permitted, context);
    if (!backups || !backups->preserveUnbound(hash, nativeReader, selected.generation, selected.course) ||
        !backups->unboundComplete() || !backups->closeReaders()) {
      LOG_ERR("COMPANION", "Legacy review backup preservation refused");
      return 0;
    }
  }
  return readHalCourseBaselineReviewPage(store, permitted, context, hash, nativeReader, selected.generation,
                                         selected.course, selected.offset, selected.limit, scratch, response);
}
}  // namespace companion

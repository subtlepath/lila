#pragma once

#if LILA_TINTA
#include "CompanionCourseBinding.h"
#include "HalCompanionHeapAdmission.h"
#include "HalCourseStateMigration.h"
#include "HalTransferStorage.h"

namespace companion {
// Caller excludes learner/content writers and retains permission through admission.
inline bool prepareBoundCourseRemovalState(HalTransferStorage& storage, const ContentManifest& expected,
                                           std::span<uint8_t> io, bool (*permitted)(void*), void* context) {
  if (!permitted || !permitted(context) || io.size() < 512 || !validCourseBinding(expected) ||
      expected.formatVersion != 1 ||
      !admitCompanionHeap(sizeof(tinta::core::pack::Pack), sizeof(tinta::core::pack::Pack))) {
    LOG_ERR("COMPANION", "Course removal preparation admission failed");
    return false;
  }
  // The retained transfer parser is reused; its fixed fields exceed the stack budget.
  if (!storage.validateContent(ACTIVE_COURSE_PATH, ACTIVE_COURSE_PATH, expected, io) || !permitted(context) ||
      !admitCompanionHeap() || !prepareMigratedCourseState(storage, expected.logicalIdentity, io) ||
      !permitted(context) || !admitCompanionHeap()) {
    LOG_ERR("COMPANION", "Course removal pack/state preparation failed");
    return false;
  }
  return true;
}
}  // namespace companion
#endif

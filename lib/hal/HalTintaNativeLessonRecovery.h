#pragma once

#include "CompanionTintaPackSubjectCatalog.h"
#include "HalTintaCompletionSetView.h"
#include "HalTintaDerivedFileVerification.h"
#include "core/profile/LessonCompletion.h"

namespace companion {
// Caller validates immutable pack structure and lesson-identity uniqueness.
inline bool projectTintaNativeLessons(tinta::core::Profile& profile, HalTintaCompletionSetView& completions,
                                      const tinta::core::pack::Pack& pack, InventoryHashProgress progress = nullptr,
                                      void* progressContext = nullptr) {
  if (progress && !progress(progressContext)) {
    LOG_ERR("COMPANION", "Native lesson recovery: cancelled");
    return false;
  }
  TintaPackSubjectKeys lessons(pack, false);
  uint32_t count = 0;
  if (!pack.isOpen() || lessons.count() > UINT16_MAX || !completions.entryCount(TintaCompletionKind::Lessons, count)) {
    LOG_ERR("COMPANION", "Native lesson recovery: pack or completion source unavailable");
    return false;
  }
  struct Context {
    TintaPackSubjectKeys& lessons;
    HalTintaCompletionSetView& completions;
    InventoryHashProgress progress;
    void* progressContext;
    uint32_t matched = 0;
  } context{lessons, completions, progress, progressContext};
  const auto completed = [](void* opaque, uint16_t index, bool& done) {
    auto& owner = *static_cast<Context*>(opaque);
    if (owner.progress && !owner.progress(owner.progressContext)) return false;
    uint32_t identity = 0;
    if (!owner.lessons.read(index, identity) || !owner.completions.contains(identity, done)) return false;
    if (done) ++owner.matched;
    return true;
  };
  auto candidate = profile;
  if (!tinta::core::LessonCompletion::project(candidate, static_cast<uint16_t>(lessons.count()), &context, completed) ||
      context.matched != count || (progress && !progress(progressContext))) {
    LOG_ERR("COMPANION", "Native lesson recovery: failed lookup or unknown completion identity");
    return false;
  }
  profile.currentLesson = candidate.currentLesson;
  profile.unlockedThrough = candidate.unlockedThrough;
  return true;
}
// Caller has loaded the profile and verified completion ownership before persistence.
inline bool persistTintaNativeLessonProjection(tinta::core::StateStore& store, tinta::core::Profile& profile,
                                               HalTintaCompletionSetView& completions,
                                               const tinta::core::pack::Pack& pack,
                                               InventoryHashProgress progress = nullptr,
                                               void* progressContext = nullptr) {
  if (!store.available()) {
    LOG_ERR("COMPANION", "Native lesson recovery: profile storage unavailable");
    return false;
  }
  auto candidate = profile;
  if (!projectTintaNativeLessons(candidate, completions, pack, progress, progressContext)) return false;
  if (candidate.currentLesson == profile.currentLesson && candidate.unlockedThrough == profile.unlockedThrough)
    return true;
  if (progress && !progress(progressContext)) {
    LOG_ERR("COMPANION", "Native lesson recovery: cancelled before profile replacement");
    return false;
  }
  if (!candidate.save(store)) {
    LOG_ERR("COMPANION", "Native lesson recovery: profile replacement failed");
    return false;
  }
  profile.currentLesson = candidate.currentLesson;
  profile.unlockedThrough = candidate.unlockedThrough;
  return true;
}
// Caller verifies the actual installed pack digest and excludes source writers.
inline bool projectVerifiedTintaNativeLessons(tinta::core::Profile& profile, HalFile& file,
                                              const TintaDerivedManifestView& manifest, const Identity& course,
                                              const Identity& storage, const Digest& packHash, const Digest& frontier,
                                              const tinta::core::pack::Pack& pack, std::span<uint8_t> scratch,
                                              InventoryHashProgress progress = nullptr,
                                              void* progressContext = nullptr) {
  if (!manifest.matches(course, storage, packHash, frontier) || !pack.isOpen()) {
    LOG_ERR("COMPANION", "Native lesson recovery: invalid receipt binding");
    return false;
  }
  if (verifyTintaDerivedFileReceipt(file, manifest, TintaDerivedFile::Lessons, scratch, progress, progressContext) !=
      TintaDerivedVerification::Verified)
    return false;
  HalTintaCompletionSetView completions(file);
  return completions.begin(TintaCompletionKind::Lessons, scratch, progress, progressContext) &&
         projectTintaNativeLessons(profile, completions, pack, progress, progressContext);
}
// Profile is already loaded; retain preferences while replacing only verified lesson projection.
inline bool persistVerifiedTintaNativeLessons(tinta::core::StateStore& store, tinta::core::Profile& profile,
                                              HalFile& file, const TintaDerivedManifestView& manifest,
                                              const Identity& course, const Identity& storage, const Digest& packHash,
                                              const Digest& frontier, const tinta::core::pack::Pack& pack,
                                              std::span<uint8_t> scratch, InventoryHashProgress progress = nullptr,
                                              void* progressContext = nullptr) {
  if (!store.available()) {
    LOG_ERR("COMPANION", "Native lesson recovery: profile storage unavailable");
    return false;
  }
  auto candidate = profile;
  if (!projectVerifiedTintaNativeLessons(candidate, file, manifest, course, storage, packHash, frontier, pack, scratch,
                                         progress, progressContext))
    return false;
  if (candidate.currentLesson == profile.currentLesson && candidate.unlockedThrough == profile.unlockedThrough)
    return true;
  if ((progress && !progress(progressContext)) || !candidate.save(store)) {
    LOG_ERR("COMPANION", "Native lesson recovery: verified profile replacement failed");
    return false;
  }
  profile.currentLesson = candidate.currentLesson;
  profile.unlockedThrough = candidate.unlockedThrough;
  return true;
}
}  // namespace companion

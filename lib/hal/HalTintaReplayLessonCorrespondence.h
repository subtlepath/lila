#pragma once

#include "HalTintaLegacyCourseReferences.h"
#include "HalTintaReplayStore.h"
#include "core/profile/LessonCompletion.h"

namespace companion {
// Profile and original pack are verified immutable inputs. Caller freezes the
// audited projection and excludes writers; preferences and unlock choices stay
// local. This compares lesson progress without saving or repairing the profile.
inline bool compareTintaReplayLessons(const tinta::core::Profile& profile, const tinta::core::pack::Pack& pack,
                                      HalTintaReplayStore& store, const Identity& course, bool (*permitted)(void*),
                                      void* context) {
  const auto failure = []([[maybe_unused]] const char* reason) {
    LOG_ERR("COMPANION", "Replay lesson correspondence refused: %s", reason);
    return false;
  };
  if (!permitted || !permitted(context) || !store.matchesCourse(course) ||
      !inspectTintaLegacyLessonReferences(profile, pack, permitted, context))
    return failure("binding or profile");
  TintaPackSubjectKeys lessons(pack, false);
  struct Lookup {
    TintaPackSubjectKeys& lessons;
    HalTintaReplayStore& store;
    bool (*permitted)(void*);
    void* context;
    uint32_t matched = 0;
  } lookup{lessons, store, permitted, context};
  auto completed = [](void* opaque, uint16_t index, bool& done) {
    auto& lookup = *static_cast<Lookup*>(opaque);
    uint32_t uid = 0;
    if (!lookup.permitted(lookup.context) || !lookup.lessons.read(index, uid) ||
        !lookup.store.completed(EventKind::LessonComplete, uid, done))
      return false;
    if (done) ++lookup.matched;
    if (index % 32 == 0) vTaskDelay(1);
    return true;
  };
  auto projected = profile;
  if (!tinta::core::LessonCompletion::project(projected, static_cast<uint16_t>(lessons.count()), &lookup, completed) ||
      projected.currentLesson != profile.currentLesson || projected.unlockedThrough != profile.unlockedThrough)
    return failure("profile projection");
  uint32_t key = 0, enabledCount = 0;
  bool previous = false, found = false;
  for (;;) {
    if (!permitted(context) || !store.nextKey(HalTintaReplayStore::Kind::Lesson, previous, key, key, found))
      return failure("completion enumeration");
    if (!found) break;
    bool enabled = false;
    if (!store.completed(EventKind::LessonComplete, key, enabled)) return failure("completion lookup");
    if (enabled && ++enabledCount > lookup.matched) return failure("unknown completion identity");
    previous = true;
  }
  return (enabledCount == lookup.matched && permitted(context)) || failure("count or permission");
}
}  // namespace companion

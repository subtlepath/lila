#pragma once

#include "CompanionTintaStoryIdentity.h"
#include "core/pack/Pack.h"

namespace companion {
// Caller validates the pack and checks identity uniqueness across its stories.
inline bool tintaPackStoryIdentity(const tinta::core::pack::Pack& pack, const tinta::core::pack::Story& story,
                                   uint32_t& output) {
  namespace pk = tinta::core::pack;
  uint32_t lessonIdentity = UINT32_MAX;
  if (story.lesson != pk::kNone16) {
    pk::Lesson lesson;
    pk::Unit unit;
    if (!pack.lesson(story.lesson, lesson) || !pack.unit(lesson.unit, unit) || !lesson.number) return false;
    lessonIdentity = (static_cast<uint32_t>(unit.number) << 16) | lesson.number;
    if (lessonIdentity == UINT32_MAX) return false;
  }
  TintaStoryIdentity identity;
  if (!identity.begin(static_cast<uint8_t>(story.kind), lessonIdentity)) return false;
  const auto visit = [](void* context, const uint8_t* bytes, uint32_t size) {
    return static_cast<TintaStoryIdentity*>(context)->title({bytes, size});
  };
  if (!pack.visitStr(story.title, &identity, visit)) return false;
  return identity.finish(output);
}
}  // namespace companion

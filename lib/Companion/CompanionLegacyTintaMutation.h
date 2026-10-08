#pragma once

#include "CompanionLegacyTintaJournal.h"
#include "CompanionTintaProgressMutation.h"

namespace companion {
// Owner verifies legacy replay state and resolves undoRecord to its reserved migration identity.
inline bool mapLegacyTintaMutation(const LegacyTintaEntry& entry, const tinta::core::ItemState& before,
                                   const Identity& course, const TintaSchedulerConfiguration& configuration,
                                   const EventIdentity& undoTarget, TintaProgressMutation& output) {
  if (!tinta_body_detail::nonzero(course) || !entry.uid || entry.uid == UINT32_MAX || before.uid != entry.uid)
    return false;
  TintaProgressMutation mapped;
  auto& body = mapped.bodies[0];
  body.course = course;
  body.uid = entry.uid;
  switch (entry.operation) {
    case LegacyTintaOperation::Review:
      body.kind = EventKind::Review;
      body.grade = entry.grade;
      body.format = entry.format;
      body.responseMilliseconds = static_cast<uint32_t>(entry.responseQuarterSeconds) * 250;
      body.configuration = configuration;
      if (!tinta_body_detail::valid(body)) return false;
      mapped.count = 1;
      break;
    case LegacyTintaOperation::Undo:
      body.kind = EventKind::UndoReview;
      body.undoTarget = undoTarget;
      if (!tinta_body_detail::valid(body)) return false;
      mapped.count = 1;
      break;
    case LegacyTintaOperation::Flags:
      if ((entry.flags | before.flags) & ~tinta::core::item_flag::kAll ||
          ((entry.flags ^ before.flags) & tinta::core::item_flag::kLeech))
        return false;
      body.kind = EventKind::Suspension;
      body.enabled = (entry.flags & tinta::core::item_flag::kSuspended) != 0;
      mapped.bodies[1] = body;
      mapped.bodies[1].kind = EventKind::Star;
      mapped.bodies[1].enabled = (entry.flags & tinta::core::item_flag::kStarred) != 0;
      mapped.count = 2;
      break;
    default:
      return false;
  }
  output = mapped;
  return true;
}
}  // namespace companion

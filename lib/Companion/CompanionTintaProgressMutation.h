#pragma once

#include "CompanionTintaBody.h"
#include "core/srs/ProgressStore.h"

namespace companion {
struct TintaProgressMutation {
  std::array<TintaBody, 2> bodies{};
  uint8_t count = 0;
};

// Output is caller-owned; publishing multiple bodies requires recoverable batch persistence.
inline bool mapTintaProgressMutation(const tinta::core::JournalEntry& entry, const tinta::core::ItemState& before,
                                     const tinta::core::ItemState& after, uint32_t responseMilliseconds,
                                     const Identity& course, const TintaSchedulerConfiguration& configuration,
                                     const EventIdentity& undoTarget, TintaProgressMutation& output) {
  if (!tinta_body_detail::nonzero(course) || entry.uid == 0 || entry.uid == UINT32_MAX || before.uid != entry.uid ||
      after.uid != entry.uid)
    return false;
  TintaProgressMutation mapped;
  auto& body = mapped.bodies[0];
  body.course = course;
  body.uid = entry.uid;
  if (entry.isReview()) {
    body.kind = EventKind::Review;
    body.grade = static_cast<uint8_t>(entry.grade());
    body.format = entry.format();
    body.responseMilliseconds = responseMilliseconds;
    body.configuration = configuration;
    if (!tinta_body_detail::valid(body)) return false;
    mapped.count = 1;
  } else if (entry.controlCode() == tinta::core::JournalEntry::kUndo) {
    if (responseMilliseconds != 0) return false;
    body.kind = EventKind::UndoReview;
    body.undoTarget = undoTarget;
    if (!tinta_body_detail::valid(body)) return false;
    mapped.count = 1;
  } else if (entry.controlCode() == tinta::core::JournalEntry::kSetFlags) {
    if (responseMilliseconds != 0 || after.flags != entry.arg ||
        ((before.flags | after.flags) & ~tinta::core::item_flag::kAll) ||
        ((before.flags ^ after.flags) & ~(tinta::core::item_flag::kSuspended | tinta::core::item_flag::kStarred)))
      return false;
    for (const auto flag : {tinta::core::item_flag::kSuspended, tinta::core::item_flag::kStarred}) {
      if (!((before.flags ^ after.flags) & flag)) continue;
      auto& changed = mapped.bodies[mapped.count++];
      changed.course = course;
      changed.uid = entry.uid;
      changed.kind = flag == tinta::core::item_flag::kSuspended ? EventKind::Suspension : EventKind::Star;
      changed.enabled = (after.flags & flag) != 0;
    }
  } else {
    return false;
  }
  output = mapped;
  return true;
}
}  // namespace companion

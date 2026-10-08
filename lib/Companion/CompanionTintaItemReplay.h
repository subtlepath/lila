#pragma once

#include "CompanionTintaBody.h"
#include "core/srs/Review.h"

namespace companion {
struct TintaReplayCounts {
  bool newItem = false;
  bool review = false;
  bool correct = false;
};

inline bool applyTintaItemFlags(const TintaBody& body, tinta::core::ItemState& state) {
  if (!tinta_body_detail::valid(body) || state.uid != body.uid ||
      (body.kind != EventKind::Suspension && body.kind != EventKind::Star))
    return false;
  const uint16_t bit =
      body.kind == EventKind::Suspension ? tinta::core::item_flag::kSuspended : tinta::core::item_flag::kStarred;
  state.flags = body.enabled ? state.flags | bit : state.flags & ~bit;
  return true;
}

// Caller validates envelopes, orders/deduplicates events and excludes undone reviews.
inline bool applyTintaItemReplay(const TintaBody& body, uint32_t studyDay, tinta::core::Fsrs& scheduler,
                                 tinta::core::ItemState& state, TintaReplayCounts& counts) {
  if (!tinta_body_detail::valid(body) || state.uid != body.uid || studyDay > UINT16_MAX) return false;
  TintaReplayCounts next;
  switch (body.kind) {
    case EventKind::Review:
      next.newItem = state.isNew();
      next.review = !next.newItem && state.lastDay != studyDay;
      next.correct = body.grade != static_cast<uint8_t>(tinta::core::Grade::Again);
      scheduler.configure(static_cast<float>(body.configuration.retentionBasisPoints) / 10000.0f,
                          body.configuration.maximumInterval);
      tinta::core::applyReview(scheduler, state, static_cast<tinta::core::Grade>(body.grade),
                               static_cast<tinta::core::DayNumber>(studyDay));
      break;
    case EventKind::Suspension:
    case EventKind::Star: {
      if (!applyTintaItemFlags(body, state)) return false;
      break;
    }
    default:
      return false;
  }
  counts = next;
  return true;
}
}  // namespace companion

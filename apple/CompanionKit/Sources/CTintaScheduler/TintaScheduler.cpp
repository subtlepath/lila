#include "TintaScheduler.h"

#include "../../../../lib/Companion/CompanionTintaItemReplay.h"
#include "core/srs/ItemState.h"
#include "core/srs/Review.h"

using tinta::core::ItemState;

bool tinta_scheduler_fresh(const uint32_t uid, uint8_t* output) {
  if (!output || uid == 0 || uid == UINT32_MAX) return false;
  ItemState::fresh(uid).encode(output);
  return true;
}

bool tinta_scheduler_validate(const uint8_t* input) {
  ItemState state;
  return input && ItemState::decode(input, state) && state.uid != 0 && state.uid != UINT32_MAX;
}

bool tinta_scheduler_set_flag(const uint8_t* input, bool star, bool enabled, uint8_t* output) {
  ItemState state;
  if (!input || !output || !ItemState::decode(input, state)) return false;
  companion::TintaBody body;
  body.course.fill(1);
  body.uid = state.uid;
  body.kind = star ? companion::EventKind::Star : companion::EventKind::Suspension;
  body.enabled = enabled;
  if (!companion::applyTintaItemFlags(body, state)) return false;
  state.encode(output);
  return true;
}

bool tinta_scheduler_review(const uint8_t* input, const uint8_t grade, const uint16_t day,
                            const uint16_t retentionBasisPoints, const uint16_t maximumInterval, uint8_t* output) {
  return tinta_scheduler_review_counted(input, grade, day, retentionBasisPoints, maximumInterval, output, nullptr);
}
bool tinta_scheduler_review_counted(const uint8_t* input, const uint8_t grade, const uint16_t day,
                                    const uint16_t retentionBasisPoints, const uint16_t maximumInterval,
                                    uint8_t* output, TintaReviewCounts* counts) {
  ItemState state;
  if (!input || !output || grade < 1 || grade > 4 || retentionBasisPoints == 0 || retentionBasisPoints >= 10000 ||
      maximumInterval == 0 || !ItemState::decode(input, state) || state.uid == 0 || state.uid == UINT32_MAX)
    return false;
  companion::TintaBody body;
  // This item-only bridge receives no course; replay validates event bindings before calling it.
  body.course.fill(1);
  body.uid = state.uid;
  body.grade = grade;
  body.configuration = {retentionBasisPoints, maximumInterval};
  tinta::core::Fsrs scheduler;
  companion::TintaReplayCounts replayCounts;
  if (!companion::applyTintaItemReplay(body, day, scheduler, state, replayCounts)) return false;
  state.encode(output);
  if (counts) *counts = {replayCounts.newItem, replayCounts.review, replayCounts.correct};
  return true;
}

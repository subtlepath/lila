#pragma once

#include <cstdint>

#include "core/Clock.h"
#include "core/srs/Fsrs.h"
#include "core/srs/ItemState.h"

namespace tinta::core {

// Tinta's scheduling rules around the FSRS memory model (PLAN.md 8.1).
//
// Learning steps are session-relative: a new or failed item comes back after
// at least kStepGaps[0] other items, then after kStepGaps[1], then graduates to
// day-based scheduling. Within a step, grades move as in FSRS's own learning
// steps: Again restarts the steps, Hard repeats the current one, Good moves on,
// Easy graduates at once. A new item's first grade other than Easy enters step
// 0. The memory model is updated on every grade; a same-day grade uses FSRS's
// short-term formula.
//
// This is the only place an item's state changes on a grade. ProgressStore
// calls it for live reviews and again when it replays the journal, so a
// rebuilt items.bin matches the original bit for bit.
constexpr uint8_t kLearningSteps = 2;
constexpr uint8_t kStepGaps[kLearningSteps] = {3, 8};
constexpr uint8_t kLeechLapses = 6;

struct ReviewOutcome {
  bool inSession = false;     // the item comes back later in this session
  uint8_t gap = 0;            // ...after at least this many other items
  uint16_t intervalDays = 0;  // otherwise it is due this many days from today
  bool lapse = false;         // a graduated item was forgotten
  bool becameLeech = false;   // this lapse reached kLeechLapses
};

// Applies one grade given on `day` to `state` (uid and flags are kept).
ReviewOutcome applyReview(const Fsrs& fsrs, ItemState& state, Grade grade, DayNumber day);

}  // namespace tinta::core

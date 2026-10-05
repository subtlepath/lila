#include "core/srs/Review.h"

namespace tinta::core {

ReviewOutcome applyReview(const Fsrs& fsrs, ItemState& state, Grade grade, DayNumber day) {
  const Phase phase = state.phaseKind();

  Fsrs::Memory memory;
  if (phase == Phase::New) {
    memory = fsrs.initial(grade);
  } else {
    // The X4's learner-confirmed date can move backwards; treat that as a
    // same-day review rather than a negative gap.
    const uint32_t elapsed = day > state.lastDay ? static_cast<uint32_t>(day - state.lastDay) : 0;
    memory = fsrs.next({state.stabilityDays(), state.difficultyValue()}, elapsed, grade);
  }
  state.setStabilityDays(memory.stability);
  state.setDifficultyValue(memory.difficulty);
  if (state.reps < 0xFF) ++state.reps;
  state.lastDay = day;

  ReviewOutcome out;
  bool graduate = false;
  uint8_t step = 0;
  Phase stepPhase = Phase::Learning;

  switch (phase) {
    case Phase::New:
      graduate = grade == Grade::Easy;
      break;
    case Phase::Learning:
    case Phase::Relearning:
      stepPhase = phase;
      if (grade == Grade::Again) {
        step = 0;
      } else if (grade == Grade::Hard) {
        step = state.step() < kLearningSteps ? state.step() : kLearningSteps - 1;
      } else if (grade == Grade::Good && state.step() + 1 < kLearningSteps) {
        step = static_cast<uint8_t>(state.step() + 1);
      } else {
        graduate = true;
      }
      break;
    case Phase::Review:
      if (grade == Grade::Again) {
        out.lapse = true;
        stepPhase = Phase::Relearning;
        if (state.lapses < 0xFF) ++state.lapses;
        if (state.lapses >= kLeechLapses && (state.flags & item_flag::kLeech) == 0) {
          state.flags |= item_flag::kLeech;
          out.becameLeech = true;
        }
      } else {
        graduate = true;
      }
      break;
  }

  if (graduate) {
    out.intervalDays = fsrs.interval(memory.stability);
    const uint32_t due = static_cast<uint32_t>(day) + out.intervalDays;
    state.dueDay = due > 0xFFFF ? 0xFFFF : static_cast<DayNumber>(due);
    state.setPhase(Phase::Review);
  } else {
    out.inSession = true;
    out.gap = kStepGaps[step];
    state.dueDay = day;
    state.setPhase(stepPhase, step);
  }
  return out;
}

}  // namespace tinta::core

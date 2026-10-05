#pragma once

#include <cstdint>

#include "core/Clock.h"
#include "core/pack/Pack.h"
#include "core/srs/Fsrs.h"
#include "core/srs/ProgressStore.h"

namespace tinta::core::library {

// The word the sleep screen teaches (PLAN.md 4.7, M6): one of the learner's
// weakest words, so the image left on the glass is worth reading.
//
// Candidates are the recognise items already learnt (graded, not suspended,
// not vulgar), weakest first by retrievability today. The pick is one of the
// kPool weakest, chosen by `seed` (the day and how many times the device has
// slept), so it is the same for the same day and sleep and moves on with the
// next. A learner with nothing learnt yet gets a new word of the current
// lesson instead (the first that is not vulgar), and someone past the last
// lesson with nothing learnt gets none.
struct SleepWord {
  int32_t item = -1;  // catalog index of a VocabRecognise item, or -1
  bool learnt = false;
};

inline constexpr uint8_t kSleepWordPool = 5;

SleepWord pickSleepWord(const pack::Pack& pack, ProgressStore& progress, const Fsrs& fsrs, DayNumber today,
                        uint32_t seed, uint16_t currentLesson);

// Reviews due by `day` that are not done yet: items due that day or earlier
// (overdue included), new and suspended items left out. For "tomorrow".
uint32_t dueBy(ProgressStore& progress, DayNumber today, DayNumber day);

}  // namespace tinta::core::library

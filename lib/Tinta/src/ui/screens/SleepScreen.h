#pragma once

// The card left on the panel when lila sleeps from inside Tinta with its
// Current Page sleep screen (PLAN.md 4.7): one of the learner's weakest words
// (Spanish with its article, respelling, gloss, an example and its English)
// under the date, then the streak and tomorrow's reviews. lila adds its moon.

#include <FreeInkUIDisplayTarget.h>

#include "core/Clock.h"
#include "core/pack/Pack.h"
#include "ui/Theme.h"

namespace tinta::ui {

struct SleepInfo {
  bool dayKnown = false;
  core::DayNumber day = 0;
  // The word (core/library/SleepWord).
  const core::pack::Pack* pack = nullptr;
  uint16_t lemma = 0;
  bool learnt = false;  // one of the weakest; else a word of the current lesson
  bool countsKnown = false;
  uint16_t streak = 0;
  uint32_t dueTomorrow = 0;
};

void drawSleepScreen(freeink::ui::DisplayTarget& target, const Theme& theme, const SleepInfo& info);

}  // namespace tinta::ui

#pragma once

#include <cstdint>

#include "core/pack/Pack.h"

namespace tinta::core::session {

// The items a lesson's practice runs through (PLAN.md 4.1, M5): the
// recognise items of the lesson's new words, then its items that need
// nothing learnt first (word order, clozes on words met before). The rest of
// the lesson's items (gender, produce, conjugation, clozes) wait for the
// daily session, which brings each in once its word is known (DayQueue's
// prerequisite rule). Vulgar items are left out unless `showVulgar`.
// Returns the number written to `out` (at most `cap`).
uint16_t lessonPractice(const pack::Pack& pack, uint16_t lesson, bool showVulgar, uint32_t* out, uint16_t cap);

}  // namespace tinta::core::session

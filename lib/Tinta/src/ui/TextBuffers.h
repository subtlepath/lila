#pragma once

// One span buffer and one run buffer for the screens that typeset more than a
// card at a time: a lesson's note pages and the reader. Only the screen on
// top draws, so they share: at 192 spans and 160 runs, about 6 KB of heap
// while Tinta is open.

#include "core/text/Typesetter.h"

namespace tinta::ui {

inline constexpr uint16_t kSharedSpanCap = 192;
inline constexpr uint16_t kSharedRunCap = 160;

// From App::open() to App's end; false when out of memory.
bool openTextBuffers();
void closeTextBuffers();

core::text::Span* sharedSpans();
core::text::Run* sharedRuns();

}  // namespace tinta::ui

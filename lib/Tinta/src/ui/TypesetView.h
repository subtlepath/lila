#pragma once

// Draws core::text::Typesetter output through FreeInkUI's DisplayTarget
// (PLAN.md section 5.5). DisplayTarget draws text only through slot fonts and
// text(rect, string, style), so each run re-points the scratch slot at its
// span's font and passes a one-line rect whose top is baseline - ascent:
// runs in different fonts then land on the same baseline, and a rect as wide
// as the measured run never wraps or ellipsises.

#include <FreeInkUIDisplayTarget.h>

#include "core/text/Typesetter.h"

namespace tinta::ui {

// Draws every run of the last layout, shifted by (dx, dy). Afterwards the
// scratch slot points at the last run's font.
void drawTypeset(freeink::ui::DisplayTarget& target, const core::text::Typesetter& typesetter, int16_t dx = 0,
                 int16_t dy = 0);

// Draws one run with its span's decorations: underline, cloze blank (a rule
// in place of the text) or highlight (white text in a black box covering the
// line).
void drawRun(freeink::ui::DisplayTarget& target, const core::text::Span& span, const core::text::Run& run,
             int16_t dx = 0, int16_t dy = 0);

}  // namespace tinta::ui

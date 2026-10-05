#pragma once

// Per-device look: FreeInkUI theme tokens, the safe area from the board
// profile's bezel insets, and the sizes of Tinta's own chrome. Key devices
// get compact rows and a strong focus mark (black fill), because focus is how
// they navigate; the X4 Pro gets finger-sized rows, and focus there is only
// the brief gray flash after a tap.

#include <FreeInkUIDisplayTarget.h>

namespace tinta::ui {

struct Theme {
  freeink::ui::ThemeTokens tokens{};
  bool touch = false;

  int16_t statusHeight = 40;
  int16_t hintsHeight = 44;   // key-hint footer (key devices)
  int16_t choiceHeight = 64;  // choice bar: label and an optional detail line
  int16_t rowHeight = 44;     // menu and form rows
  int16_t margin = 12;        // text inset from the safe area's sides
  int16_t gap = 8;
};

// Builds the theme for `device` (logical portrait context, with the safe area
// and touch already set) and points every font slot at Tinta's fonts, so the
// SDK's bundled Noto Sans is never drawn.
void buildTheme(Theme& theme, freeink::ui::DisplayTarget& target, const freeink::ui::DeviceContext& device);

}  // namespace tinta::ui

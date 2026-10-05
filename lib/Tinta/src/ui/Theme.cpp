#include "ui/Theme.h"

#include "ui/Fonts.h"

namespace tinta::ui {
namespace {

using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::StyleSet;

// Slot 6 is not part of the font plan; pointing it at chrome text keeps the
// SDK's default font out of every slot.
constexpr freeink::ui::FontId kSlotSpare = 6;

StyleSet keyRowStyles() {
  StyleSet s = freeink::ui::defaultListRowStyles();
  // Focus is the cursor on key devices: it must read at a glance.
  s.focused.background = Paint::solid(Color::Black);
  s.focused.foreground = Paint::solid(Color::White);
  s.active = s.focused;
  s.selected.background = Paint::solid(Color::White);
  s.selected.foreground = Paint::solid(Color::Black);
  s.selected.border = Paint::solid(Color::Black);
  s.selected.borderWidth = 2;
  return s;
}

StyleSet keyButtonStyles() {
  StyleSet s = freeink::ui::outlinedButtonStyles(0, Color::White);
  s.selected.borderWidth = 2;
  s.focused.background = Paint::solid(Color::Black);
  s.focused.foreground = Paint::solid(Color::White);
  s.focused.border = Paint::solid(Color::Black);
  s.focused.borderWidth = 1;
  s.active = s.focused;
  s.disabled.background = Paint::solid(Color::White);
  s.disabled.foreground = Paint::dither(Color::LightGray);
  s.disabled.border = Paint::dither(Color::LightGray);
  s.disabled.borderWidth = 1;
  return s;
}

StyleSet touchRowStyles() {
  StyleSet s = freeink::ui::defaultListRowStyles();
  // Focus only shows as the tap flash; pressed is black.
  s.focused.background = Paint::dither(Color::LightGray);
  s.focused.foreground = Paint::solid(Color::Black);
  s.active.background = Paint::solid(Color::Black);
  s.active.foreground = Paint::solid(Color::White);
  s.selected.background = Paint::solid(Color::White);
  s.selected.foreground = Paint::solid(Color::Black);
  s.selected.border = Paint::solid(Color::Black);
  s.selected.borderWidth = 2;
  return s;
}

StyleSet touchButtonStyles() {
  StyleSet s = freeink::ui::outlinedButtonStyles(6, Color::LightGray);
  s.focused.background = Paint::dither(Color::LightGray);
  s.focused.foreground = Paint::solid(Color::Black);
  s.focused.border = Paint::solid(Color::Black);
  s.focused.borderWidth = 1;
  s.focused.radius = 6;
  s.active.background = Paint::solid(Color::Black);
  s.active.foreground = Paint::solid(Color::White);
  s.active.radius = 6;
  s.disabled.background = Paint::solid(Color::White);
  s.disabled.foreground = Paint::dither(Color::LightGray);
  s.disabled.border = Paint::dither(Color::LightGray);
  s.disabled.borderWidth = 1;
  s.disabled.radius = 6;
  return s;
}

}  // namespace

void buildTheme(Theme& theme, freeink::ui::DisplayTarget& target, const freeink::ui::DeviceContext& device) {
  applyFontSlots(target);
  target.setFont(kSlotSpare, font(FontRole::ChromeSmall));

  theme.touch = device.hasTouch;
  freeink::ui::ThemeTokens& t = theme.tokens;
  t = freeink::ui::defaultThemeTokens(kSlotSmall, kSlotBody, kSlotTitle);
  t.titleText.bold = false;  // the title slot is already a bold strike

  theme.rowHeight = theme.touch ? 60 : 44;
  theme.statusHeight = 40;
  theme.hintsHeight = 44;
  theme.choiceHeight = 64;
  theme.margin = 12;
  theme.gap = 8;

  t.spaceXs = 2;
  t.spaceSm = 4;
  t.spaceMd = 8;
  t.spaceLg = 16;
  t.minTouchSize = theme.touch ? 48 : 44;
  t.rowHeight = theme.rowHeight;
  t.headerHeight = 48;
  t.footerHeight = theme.hintsHeight;
  t.listMinRowHeight = theme.rowHeight;
  t.listRowPaddingY = 8;
  t.listTouchRowPaddingY = 12;
  t.listTouchMinRowHeight = theme.rowHeight;
  t.listTouchRowGap = 4;
  t.listSidePadding = theme.margin;
  t.headerSidePadding = theme.margin;
  t.headerUnderline = 2;
  t.controlRadius = theme.touch ? 10 : 0;
  t.sheetRadius = theme.touch ? 12 : 0;
  t.capsuleRadius = 255;
  t.listRow = theme.touch ? touchRowStyles() : keyRowStyles();
  t.button = theme.touch ? touchButtonStyles() : keyButtonStyles();
}

}  // namespace tinta::ui

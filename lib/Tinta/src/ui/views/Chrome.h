#pragma once

// Tinta's chrome: the status bar, the footer over the four front keys (as a
// choice bar or as key hints), and banners.

#include <stdint.h>

#include "app/View.h"
#include "platform/Board.h"
#include "ui/KeyMap.h"
#include "ui/Theme.h"

namespace freeink {
struct Icon;
}

namespace tinta::ui {

struct StatusInfo {
  const char* title = nullptr;
  const char* time = nullptr;  // null without a time of day
  platform::BatteryReading battery{};
  bool back = false;  // touch: a back arrow before the title, tapped for kActionBack
};

void drawStatusBar(app::UiScreen& screen, freeink::ui::Rect rect, const Theme& theme, const StatusInfo& info);

enum class FooterStyle : uint8_t {
  Choice,  // answers: ruled cells, labels in body bold, an optional detail line
  Hints,   // what each front key does: small labels or icons
};

// Draws a footer band over the front keys at `rect`'s y and height. Cells use
// the KeyMap's geometry, so each sits over its key. With `registerTaps`, each
// non-empty, enabled cell is a touch target for kActionChoice(i).
void drawFooter(app::UiScreen& screen, freeink::ui::Rect rect, const Theme& theme, const KeyMap& keys,
                const ChoiceBar& bar, FooterStyle style, bool registerTaps);

// A boxed notice with a warning icon, taken from the top of the body.
void drawBanner(app::UiScreen& screen, const Theme& theme, const char* text);

// An icon centred in `rect`, in `color`.
void drawIcon(freeink::ui::DrawTarget& target, freeink::ui::Rect rect, const freeink::Icon& icon,
              freeink::ui::Color color = freeink::ui::Color::Black);

}  // namespace tinta::ui

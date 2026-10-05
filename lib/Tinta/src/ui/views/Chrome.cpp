#include "ui/views/Chrome.h"

#include <FreeInkUIIcon.h>
#include <stdio.h>

#include "icons/Icons.h"
#include "ui/Fonts.h"

namespace tinta::ui {
namespace {

using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;

const Paint kInk = Paint::solid(Color::Black);
const Paint kPaper = Paint::solid(Color::White);

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

TextStyle style(freeink::ui::FontId font, TextAlign align = TextAlign::Left, Color color = Color::Black,
                uint8_t maxLines = 1) {
  TextStyle s;
  s.font = font;
  s.align = align;
  s.color = color;
  s.maxLines = maxLines;
  return s;
}

// A battery outline with a fill for the charge and a nub on the right.
int16_t drawBattery(freeink::ui::DrawTarget& t, int16_t right, int16_t cy, const platform::BatteryReading& b) {
  constexpr int16_t kW = 26;
  constexpr int16_t kH = 13;
  const Rect body{i16(right - kW - 3), i16(cy - kH / 2), kW, kH};
  t.stroke(body, kInk, 2);
  t.fill(Rect{body.right(), i16(cy - 3), 3, 6}, kInk);
  if (b.percentKnown) {
    const int16_t inner = i16(kW - 6);
    const int16_t fill = i16((inner * b.percent + 50) / 100);
    if (fill > 0) t.fill(Rect{i16(body.x + 3), i16(body.y + 3), fill, i16(kH - 6)}, kInk);
  }
  return body.x;
}

}  // namespace

void drawIcon(freeink::ui::DrawTarget& target, const Rect rect, const freeink::Icon& icon, const Color color) {
  target.bitmap(rect, freeink::ui::bitmapFromIcon(icon), freeink::ui::BitmapMode::Center, Paint::solid(color));
}

void drawStatusBar(app::UiScreen& screen, const Rect rect, const Theme& theme, const StatusInfo& info) {
  freeink::ui::DrawTarget& t = screen.target();
  t.fill(rect, kPaper);
  t.fill(Rect{rect.x, i16(rect.bottom() - 2), rect.width, 2}, kInk);
  const Rect band{rect.x, rect.y, rect.width, i16(rect.height - 2)};
  const int16_t cy = i16(band.y + band.height / 2);

  int16_t left = i16(band.x + theme.margin);
  if (info.back) {
    drawIcon(t, Rect{i16(left - 4), band.y, 24, band.height}, icons::kChevronLeft24);
    left = i16(left + 24);
    // The whole left half is the target: the arrow alone is too small a target.
    screen.frame().hit(Rect{rect.x, rect.y, i16(rect.width / 2), rect.height}, app::kActionBack, 0,
                       freeink::ui::InputTouch);
  }

  int16_t right = i16(band.right() - theme.margin);
  if (info.battery.percentKnown) {
    right = i16(drawBattery(t, right, cy, info.battery) - 6);
    char percent[8];
    snprintf(percent, sizeof percent, "%u%%", info.battery.percent);
    const int16_t w = t.measureText(kSlotSmall, percent, style(kSlotSmall)).width;
    t.text(Rect{i16(right - w), band.y, w, band.height}, percent, style(kSlotSmall));
    right = i16(right - w - 14);
  }
  if (info.time) {
    const int16_t w = t.measureText(kSlotBodyBold, info.time, style(kSlotBodyBold)).width;
    t.text(Rect{i16(right - w), band.y, w, band.height}, info.time, style(kSlotBodyBold));
    right = i16(right - w - 14);
  }
  if (info.title && right > left) {
    t.text(Rect{left, band.y, i16(right - left), band.height}, info.title, style(kSlotBodyBold));
  }
}

void drawFooter(app::UiScreen& screen, const Rect rect, const Theme& theme, const KeyMap& keys, const ChoiceBar& bar,
                const FooterStyle footerStyle, const bool registerTaps) {
  freeink::ui::DrawTarget& t = screen.target();
  const Rect safe = screen.frame().safeRect();
  const bool choice = footerStyle == FooterStyle::Choice;
  t.fill(rect, kPaper);
  t.fill(Rect{rect.x, rect.y, rect.width, i16(choice ? 2 : 1)}, kInk);

  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    const FooterCell& geometry = keys.footerCell(i);
    // Cells follow the keys, but text stays out from under the bezel.
    int16_t x0 = geometry.x < safe.x ? safe.x : geometry.x;
    int16_t x1 = i16(geometry.x + geometry.width);
    if (x1 > safe.right()) x1 = safe.right();
    const Rect cell{x0, i16(rect.y + 2), i16(x1 - x0), i16(rect.height - 2)};
    if (choice && i > 0) t.fill(Rect{geometry.x, i16(rect.y + 8), 1, i16(rect.height - 16)}, kInk);

    const CellSpec& spec = bar.cells[i];
    if (spec.empty()) continue;
    const Color color = spec.enabled ? Color::Black : Color::LightGray;
    if (choice && registerTaps && spec.enabled) {
      screen.frame().hit(Rect{geometry.x, rect.y, geometry.width, rect.height}, app::kActionChoice, i,
                         freeink::ui::InputTouch);
    }

    const freeink::ui::FontId labelFont = choice ? kSlotBodyBold : kSlotSmall;
    const int16_t labelH = t.lineHeight(labelFont);
    const int16_t detailH = spec.detail ? t.lineHeight(kSlotSmall) : 0;
    const int16_t iconH = spec.icon ? static_cast<int16_t>(spec.icon->h) : 0;
    const int16_t contentH = i16((spec.label ? labelH : 0) + iconH + detailH + (spec.label && spec.icon ? 2 : 0));
    int16_t y = i16(cell.y + (cell.height - contentH) / 2);
    if (spec.icon) {
      Rect iconRect{cell.x, y, cell.width, iconH};
      t.bitmap(iconRect, freeink::ui::bitmapFromIcon(*spec.icon), freeink::ui::BitmapMode::Center,
               spec.enabled ? kInk : Paint::dither(Color::LightGray));
      y = i16(y + iconH + (spec.label ? 2 : 0));
    }
    if (spec.label) {
      TextStyle s = style(labelFont, TextAlign::Center, color);
      t.text(Rect{i16(cell.x + 2), y, i16(cell.width - 4), labelH}, spec.label, s);
      y = i16(y + labelH);
    }
    if (spec.detail) {
      t.text(Rect{i16(cell.x + 2), y, i16(cell.width - 4), detailH}, spec.detail,
             style(kSlotSmall, TextAlign::Center, color));
    }
  }
  (void)theme;
}

void drawBanner(app::UiScreen& screen, const Theme& theme, const char* text) {
  freeink::ui::DrawTarget& t = screen.target();
  const int16_t lineH = t.lineHeight(kSlotSmall);
  const Rect body = screen.body();
  const int16_t textW = i16(body.width - 2 * theme.margin - 24 - 3 * theme.gap);
  const freeink::ui::Size size =
      freeink::ui::measureWrappedText(t, text, style(kSlotSmall, TextAlign::Left, Color::Black, 3), textW);
  const int16_t h = i16((size.height > lineH ? size.height : lineH) + 2 * theme.gap + 4);
  const Rect box = screen.takeTop(h, theme.gap);
  const Rect inner{i16(box.x + theme.margin / 2), box.y, i16(box.width - theme.margin), box.height};
  t.fill(inner, kInk);
  drawIcon(t, Rect{i16(inner.x + theme.gap), inner.y, 24, inner.height}, icons::kWarning24, Color::White);
  t.text(Rect{i16(inner.x + 24 + 2 * theme.gap), inner.y, textW, inner.height}, text,
         style(kSlotSmall, TextAlign::Left, Color::White, 3));
}

}  // namespace tinta::ui

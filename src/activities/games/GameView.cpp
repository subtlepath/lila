#include "GameView.h"

#include <algorithm>
#include <string>

#include "fontIds.h"

extern const GameView CONNECT_FOUR_VIEW;
extern const GameView DOTS_AND_BOXES_VIEW;
extern const GameView LIARS_DICE_VIEW;
extern const GameView MURDER_MYSTERY_VIEW;

const GameView* gameViewFor(const table::GameId id) {
  switch (id) {
    case table::GameId::ConnectFour:
      return &CONNECT_FOUR_VIEW;
    case table::GameId::DotsAndBoxes:
      return &DOTS_AND_BOXES_VIEW;
    case table::GameId::LiarsDice:
      return &LIARS_DICE_VIEW;
    case table::GameId::MurderMystery:
      return &MURDER_MYSTERY_VIEW;
    case table::GameId::None:
      break;
  }
  return nullptr;
}

const char* gameName(const table::GameId id) {
  const GameView* view = gameViewFor(id);
  return view ? I18N.get(view->name) : "";
}

void GameTargets::add(const int x, const int y, const int w, const int h, const int value) {
  if (count >= MAX || w <= 0 || h <= 0) return;
  items[count++] = {static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h),
                    static_cast<int16_t>(value)};
}

int GameTargets::hit(const int x, const int y) const {
  for (int i = count - 1; i >= 0; i--) {
    const Target& t = items[i];
    if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) return t.value;
  }
  return -1;
}

namespace gameui {

int labelFont() { return UI_10_FONT_ID; }
int boldFont() { return UI_12_FONT_ID; }

void drawDisc(const GfxRenderer& r, const int x, const int y, const int d, const bool filled) {
  if (filled) {
    r.fillRoundedRect(x, y, d, d, d / 2, Color::Black);
  } else {
    r.drawRoundedRect(x, y, d, d, std::max(2, d / 12), d / 2, true);
  }
}

void drawDie(const GfxRenderer& r, const int x, const int y, const int size, const uint8_t face,
             const bool highlighted) {
  const int radius = std::max(3, size / 6);
  r.fillRoundedRect(x, y, size, size, radius, Color::White);
  if (face == 0) {
    // Hidden die: a dithered face reads as "unknown" at any size.
    const int inset = std::max(3, size / 7);
    r.fillRoundedRect(x + inset, y + inset, size - 2 * inset, size - 2 * inset, std::max(1, radius - inset / 2),
                      Color::LightGray);
    r.drawRoundedRect(x, y, size, size, std::max(1, size / 18), radius, true);
    return;
  }
  // Dice that count toward a challenged bid get a heavy frame. The face stays
  // white so the pips remain legible down to small sizes.
  const int border = highlighted ? std::max(3, size / 8) : std::max(1, size / 18);
  r.drawRoundedRect(x, y, size, size, border, radius, true);
  // Pip positions on a 3x3 grid (bit = cell, row-major), centred on the
  // die's quarter lines.
  static constexpr uint16_t PIPS[7] = {0, 0x010, 0x101, 0x111, 0x145, 0x155, 0x16D};
  const int pip = std::max(3, size / 6);
  for (int cell = 0; cell < 9; cell++) {
    if (((PIPS[face] >> cell) & 1u) == 0) continue;
    const int cx = x + (size * (cell % 3 + 1) + 2) / 4;
    const int cy = y + (size * (cell / 3 + 1) + 2) / 4;
    r.fillRoundedRect(cx - pip / 2, cy - pip / 2, pip, pip, pip / 2, Color::Black);
  }
}

void drawTurnMarker(const GfxRenderer& r, const int x, const int centerY, const int size) {
  const int xs[3] = {x, x + size, x};
  const int ys[3] = {centerY - size / 2, centerY, centerY + size / 2};
  r.fillPolygon(xs, ys, 3, true);
}

void drawDownMarker(const GfxRenderer& r, const int centerX, const int y, const int size) {
  const int xs[3] = {centerX - size / 2, centerX + size / 2, centerX};
  const int ys[3] = {y, y, y + size * 2 / 3};
  r.fillPolygon(xs, ys, 3, true);
}

void drawFittedText(const GfxRenderer& r, const int fontId, const int x, const int y, const char* text,
                    const int maxWidth, const bool black, const EpdFontFamily::Style style) {
  if (!text || maxWidth <= 0) return;
  if (r.getTextWidth(fontId, text, style) <= maxWidth) {
    r.drawText(fontId, x, y, text, black, style);
    return;
  }
  const std::string fitted = r.truncatedText(fontId, text, maxWidth, style);
  r.drawText(fontId, x, y, fitted.c_str(), black, style);
}

}  // namespace gameui

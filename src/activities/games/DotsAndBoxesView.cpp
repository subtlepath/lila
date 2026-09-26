#include <DotsAndBoxes.h>

#include <algorithm>
#include <cstdio>

#include "GameView.h"

using table::DotsAndBoxes;

namespace {

constexpr int W = DotsAndBoxes::BOXES_W;
constexpr int H = DotsAndBoxes::BOXES_H;

// Cursor: row k in 0..H walks the horizontals on dot-row k and the verticals
// hanging below it, so Left/Right zig-zag along one band of lines and every
// line is reachable with four directions:
//   column j odd  -> horizontal (k, (j - 1) / 2)
//   column j even -> vertical   (k, j / 2)   (rows 0..H-1 only)
int lineAt(const int k, const int j) {
  if (j % 2 == 1) return DotsAndBoxes::hLine(k, (j - 1) / 2);
  return k < H ? DotsAndBoxes::vLine(k, j / 2) : -1;
}

void cursorForLine(const int line, GameCursor& cursor) {
  if (line < DotsAndBoxes::H_LINES) {
    cursor.a = static_cast<int8_t>(line / W);
    cursor.b = static_cast<int8_t>(2 * (line % W) + 1);
  } else {
    const int v = line - DotsAndBoxes::H_LINES;
    cursor.a = static_cast<int8_t>(v / (W + 1));
    cursor.b = static_cast<int8_t>(2 * (v % (W + 1)));
  }
}

void clampCursor(GameCursor& cursor) {
  cursor.a = static_cast<int8_t>(std::clamp<int>(cursor.a, 0, H));
  cursor.b = static_cast<int8_t>(std::clamp<int>(cursor.b, 0, 2 * W));
  // The bottom band has no verticals.
  if (cursor.a == H && cursor.b % 2 == 0)
    cursor.b = static_cast<int8_t>(cursor.b == 2 * W ? cursor.b - 1 : cursor.b + 1);
}

void syncCursor(const table::Game&, int, GameCursor& cursor) {
  if (cursor.signature != 0) {
    cursor.signature = 0;
    cursor.a = 0;
    cursor.b = 1;
  }
  clampCursor(cursor);
}

bool drawSelected(const DotsAndBoxes& game, const bool myTurn, const GameCursor& cursor, uint8_t* action,
                  size_t& actionLen) {
  const int line = lineAt(cursor.a, cursor.b);
  if (!myTurn || line < 0 || game.lineDrawn(line)) return false;
  action[0] = static_cast<uint8_t>(line);
  actionLen = 1;
  return true;
}

bool handleInput(const MappedInputManager& input, const table::Game& g, int, const bool myTurn, GameCursor& cursor,
                 uint8_t* action, size_t& actionLen) {
  const auto& game = static_cast<const DotsAndBoxes&>(g);
  using Button = MappedInputManager::Button;
  bool moved = true;
  if (input.wasPressed(Button::ScreenLeft)) {
    cursor.b = static_cast<int8_t>(cursor.b - (cursor.a == H ? 2 : 1));
    if (cursor.b < 0) cursor.b = static_cast<int8_t>(cursor.a == H ? 2 * W - 1 : 2 * W);
  } else if (input.wasPressed(Button::ScreenRight)) {
    cursor.b = static_cast<int8_t>(cursor.b + (cursor.a == H ? 2 : 1));
    if (cursor.b > 2 * W) cursor.b = static_cast<int8_t>(cursor.a == H ? 1 : 0);
  } else if (input.wasPressed(Button::ScreenUp)) {
    cursor.a = static_cast<int8_t>(cursor.a == 0 ? H : cursor.a - 1);
  } else if (input.wasPressed(Button::ScreenDown)) {
    cursor.a = static_cast<int8_t>(cursor.a == H ? 0 : cursor.a + 1);
  } else {
    moved = false;
  }
  if (moved) {
    clampCursor(cursor);
    return true;
  }
  if (input.wasReleased(Button::Confirm)) return drawSelected(game, myTurn, cursor, action, actionLen);
  return false;
}

// First tap selects a line, a second tap on it draws it.
bool handleTap(const table::Game& g, int, const bool myTurn, const int value, GameCursor& cursor, uint8_t* action,
               size_t& actionLen) {
  const auto& game = static_cast<const DotsAndBoxes&>(g);
  if (value < 0 || value >= DotsAndBoxes::LINES) return false;
  if (lineAt(cursor.a, cursor.b) == value) return drawSelected(game, myTurn, cursor, action, actionLen);
  cursorForLine(value, cursor);
  return true;
}

// Short labels marking whose boxes are whose. A name's first letter when no
// other player shares it; otherwise that letter plus the name's last digit
// ("CPU 1" -> "C1") or second letter, falling back to the seat number.
void seatLabels(const GameViewContext& ctx, char labels[][4]) {
  const int seats = ctx.game.seatCount();
  const auto upper = [](const char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; };
  const auto alnum = [](const char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  for (int s = 0; s < seats; s++) {
    const char* name = ctx.seatNames[s] ? ctx.seatNames[s] : "";
    const char first = upper(name[0]);
    labels[s][0] = '\0';
    if (!alnum(first)) {
      snprintf(labels[s], 4, "%d", s + 1);
      continue;
    }
    bool shared = false;
    for (int other = 0; other < seats; other++) {
      const char* otherName = ctx.seatNames[other] ? ctx.seatNames[other] : "";
      shared = shared || (other != s && upper(otherName[0]) == first);
    }
    labels[s][0] = first;
    labels[s][1] = '\0';
    if (!shared) continue;
    const size_t len = strlen(name);
    const char last = len > 0 ? name[len - 1] : '\0';
    const char second = len > 1 ? name[1] : '\0';
    labels[s][1] = (last >= '0' && last <= '9') ? last : (alnum(second) ? second : static_cast<char>('1' + s));
    labels[s][2] = '\0';
  }
  // Anything still ambiguous falls back to seat numbers.
  for (int s = 0; s < seats; s++) {
    for (int other = s + 1; other < seats; other++) {
      if (strcmp(labels[s], labels[other]) == 0) snprintf(labels[other], 4, "%d", other + 1);
    }
  }
}

void lineRect(const int line, const int x0, const int y0, const int cell, const int thick, int& x, int& y, int& w,
              int& h) {
  if (line < DotsAndBoxes::H_LINES) {
    x = x0 + (line % W) * cell;
    y = y0 + (line / W) * cell - thick / 2;
    w = cell;
    h = thick;
  } else {
    const int v = line - DotsAndBoxes::H_LINES;
    x = x0 + (v % (W + 1)) * cell - thick / 2;
    y = y0 + (v / (W + 1)) * cell;
    w = thick;
    h = cell;
  }
}

void drawScoreRow(const GfxRenderer& r, const GameViewContext& ctx, const DotsAndBoxes& game, const char* label,
                  const int seat, const int x, const int y, const int width) {
  const int font = gameui::labelFont();
  const int lineH = r.getLineHeight(font);
  int cx = x;
  if (game.currentSeat() == seat) gameui::drawTurnMarker(r, cx, y + lineH / 2, lineH / 2);
  cx += lineH / 2 + 8;
  r.drawText(font, cx, y, label, true, EpdFontFamily::BOLD);
  cx += r.getTextWidth(font, "WW", EpdFontFamily::BOLD) + 8;
  char score[8];
  snprintf(score, sizeof(score), "%d", game.score(static_cast<uint8_t>(seat)));
  const int sw = r.getTextWidth(font, score, EpdFontFamily::BOLD);
  const bool winner = game.over() && (game.winnerMask() >> seat) & 1u;
  const bool gone = !game.seatActive(static_cast<uint8_t>(seat));
  gameui::drawFittedText(r, font, cx, y, gone ? tr(STR_GAMES_TAG_LEFT) : ctx.seatNames[seat], x + width - sw - 8 - cx,
                         true, winner ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  r.drawText(font, x + width - sw, y, score, true, EpdFontFamily::BOLD);
}

void render(GfxRenderer& r, const Rect& area, const GameViewContext& ctx) {
  const auto& game = static_cast<const DotsAndBoxes&>(ctx.game);
  const int font = gameui::labelFont();
  const int lineH = r.getLineHeight(font);
  const int seats = game.seatCount();
  char labels[table::MAX_SLOTS][4] = {};
  seatLabels(ctx, labels);

  // Portrait: two-column scoreboard under the board. Landscape: one column
  // beside it, so the board can use the full height.
  const bool wide = area.width > area.height;
  const int rowStep = lineH + 6;
  const int sideW = wide ? std::min(area.width * 2 / 5, 300) : 0;
  const int scoreH = wide ? 0 : ((seats + 1) / 2) * rowStep + 8;
  const int margin = 14;
  const int boardAreaW = area.width - sideW;
  const int boardAreaH = area.height - scoreH;
  const int cell = std::min((boardAreaW - 2 * margin) / W, (boardAreaH - 2 * margin) / H);
  const int boardW = cell * W;
  const int boardH = cell * H;
  const int x0 = area.x + (boardAreaW - boardW) / 2;
  const int y0 = area.y + (boardAreaH - boardH) / 2;
  const int thick = std::max(3, cell / 14);
  const int dot = thick + 5;
  const int boxFont = cell >= 44 ? gameui::boldFont() : font;
  const int boxLineH = r.getLineHeight(boxFont);

  // Owned boxes: the owner's label, and a light wash on the local player's.
  for (int row = 0; row < H; row++) {
    for (int col = 0; col < W; col++) {
      const uint8_t owner = game.boxOwner(row, col);
      if (owner == DotsAndBoxes::NO_OWNER) continue;
      const int bx = x0 + col * cell;
      const int by = y0 + row * cell;
      if (owner == ctx.localSeat)
        r.fillRectDither(bx + thick, by + thick, cell - thick, cell - thick, Color::LightGray);
      const char* label = labels[owner < table::MAX_SLOTS ? owner : 0];
      const int tw = r.getTextWidth(boxFont, label, EpdFontFamily::BOLD);
      r.drawText(boxFont, bx + (cell - tw) / 2, by + (cell - boxLineH) / 2, label, true, EpdFontFamily::BOLD);
    }
  }

  const int last = game.lastLine();
  for (int line = 0; line < DotsAndBoxes::LINES; line++) {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    lineRect(line, x0, y0, cell, thick, x, y, w, h);
    if (game.lineDrawn(line)) {
      // The most recent line is drawn heavier so a returning player spots it.
      if (line == last) {
        const int extra = 2;
        r.fillRect(x - (h > w ? extra : 0), y - (w > h ? extra : 0), w + (h > w ? 2 * extra : 0),
                   h + (w > h ? 2 * extra : 0), true);
      } else {
        r.fillRect(x, y, w, h, true);
      }
      continue;
    }
    // Open lines: a faint dotted guide, plus a tap target around the line.
    const bool horizontal = w > h;
    const int cx = x + w / 2;
    const int cy = y + h / 2;
    for (int t = dot; t < cell - dot; t += 5) {
      if (horizontal) {
        r.drawPixel(x + t, cy, true);
      } else {
        r.drawPixel(cx, y + t, true);
      }
    }
    const int reach = cell / 3;
    if (horizontal) {
      ctx.targets.add(x + dot / 2, cy - reach, cell - dot, 2 * reach, line);
    } else {
      ctx.targets.add(cx - reach, y + dot / 2, 2 * reach, cell - dot, line);
    }
  }

  for (int row = 0; row <= H; row++) {
    for (int col = 0; col <= W; col++) {
      r.fillRoundedRect(x0 + col * cell - dot / 2, y0 + row * cell - dot / 2, dot, dot, dot / 2, Color::Black);
    }
  }

  // Cursor: an open frame around the selected line, heavier on your turn.
  if (ctx.localSeat >= 0 && !game.over()) {
    const int k = ctx.cursor.a;
    const int j = ctx.cursor.b;
    const int pad = dot / 2 + 3;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    if (j % 2 == 1) {
      x = x0 + ((j - 1) / 2) * cell + pad;
      y = y0 + k * cell - pad;
      w = cell - 2 * pad;
      h = 2 * pad;
    } else {
      x = x0 + (j / 2) * cell - pad;
      y = y0 + k * cell + pad;
      w = 2 * pad;
      h = cell - 2 * pad;
    }
    r.drawRect(x, y, w, h, ctx.myTurn ? 3 : 1, true);
  }

  if (wide) {
    const int sx = area.x + boardAreaW + 4;
    const int sy = area.y + std::max(0, (area.height - seats * rowStep) / 2);
    for (int s = 0; s < seats; s++) drawScoreRow(r, ctx, game, labels[s], s, sx, sy + s * rowStep, sideW - 16);
  } else {
    const int colW = area.width / 2;
    const int sy = y0 + boardH + margin;
    for (int s = 0; s < seats; s++) {
      drawScoreRow(r, ctx, game, labels[s], s, area.x + (s % 2) * colW + 8, sy + (s / 2) * rowStep, colW - 24);
    }
  }
}

const char* confirmLabel(const table::Game&, int, const GameCursor&) { return tr(STR_GAMES_DRAW_LINE); }

}  // namespace

extern const GameView DOTS_AND_BOXES_VIEW = {table::GameId::DotsAndBoxes,
                                             StrId::STR_GAME_DOTS_AND_BOXES,
                                             handleInput,
                                             handleTap,
                                             syncCursor,
                                             render,
                                             confirmLabel};

#include <ConnectFour.h>

#include <algorithm>

#include "GameView.h"

using table::ConnectFour;

namespace {

constexpr int COLS = ConnectFour::COLS;
constexpr int ROWS = ConnectFour::ROWS;
// Board frame thickness plus its gap to the discs.
constexpr int FRAME = 6;

int nearestOpenColumn(const ConnectFour& game, const int from) {
  for (int offset = 0; offset < COLS; offset++) {
    if (game.canDrop(from + offset)) return from + offset;
    if (game.canDrop(from - offset)) return from - offset;
  }
  return from;
}

void syncCursor(const table::Game& g, int, GameCursor& cursor) {
  const auto& game = static_cast<const ConnectFour&>(g);
  if (cursor.signature != 0) {
    cursor.signature = 0;
    cursor.a = COLS / 2;
  }
  if (!game.over() && !game.canDrop(cursor.a)) cursor.a = static_cast<int8_t>(nearestOpenColumn(game, cursor.a));
}

bool drop(const ConnectFour& game, const bool myTurn, const GameCursor& cursor, uint8_t* action, size_t& actionLen) {
  if (!myTurn || !game.canDrop(cursor.a)) return false;
  action[0] = static_cast<uint8_t>(cursor.a);
  actionLen = 1;
  return true;
}

bool handleInput(const MappedInputManager& input, const table::Game& g, int, const bool myTurn, GameCursor& cursor,
                 uint8_t* action, size_t& actionLen) {
  const auto& game = static_cast<const ConnectFour&>(g);
  using Button = MappedInputManager::Button;
  const bool left = input.wasPressed(Button::ScreenLeft) || input.wasPressed(Button::ScreenUp);
  const bool right = input.wasPressed(Button::ScreenRight) || input.wasPressed(Button::ScreenDown);
  if (left || right) {
    cursor.a = static_cast<int8_t>((cursor.a + (right ? 1 : COLS - 1)) % COLS);
    return true;
  }
  if (input.wasReleased(Button::Confirm)) return drop(game, myTurn, cursor, action, actionLen);
  return false;
}

// First tap picks the column, a second tap on it drops: a stray touch on
// e-ink should never cost a move.
bool handleTap(const table::Game& g, int, const bool myTurn, const int value, GameCursor& cursor, uint8_t* action,
               size_t& actionLen) {
  const auto& game = static_cast<const ConnectFour&>(g);
  if (value < 0 || value >= COLS) return false;
  if (cursor.a == value) return drop(game, myTurn, cursor, action, actionLen);
  cursor.a = static_cast<int8_t>(value);
  return true;
}

// Seat 0 plays solid discs, seat 1 thick rings: distinct in pure black and
// white without relying on gray dithering.
void drawPiece(const GfxRenderer& r, const int x, const int y, const int d, const uint8_t piece) {
  if (piece == 1) {
    r.fillRoundedRect(x, y, d, d, d / 2, Color::Black);
  } else if (piece == 2) {
    r.drawRoundedRect(x, y, d, d, std::max(3, d / 6), d / 2, true);
  } else {
    r.drawRoundedRect(x, y, d, d, 1, d / 2, true);
  }
}

void drawLegendEntry(const GfxRenderer& r, const GameViewContext& ctx, const ConnectFour& game, const int seat,
                     const int x, const int y, const int width) {
  const int font = gameui::labelFont();
  const int lineH = r.getLineHeight(font);
  const int sample = lineH - 4;
  int cx = x;
  if (game.currentSeat() == seat) gameui::drawTurnMarker(r, cx, y + lineH / 2, sample / 2 + 2);
  cx += sample / 2 + 8;
  drawPiece(r, cx, y + 2, sample, static_cast<uint8_t>(seat + 1));
  cx += sample + 8;
  const bool bold = game.over() && (game.winnerMask() >> seat) & 1u;
  gameui::drawFittedText(r, font, cx, y, ctx.seatNames[seat], x + width - cx, true,
                         bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}

void render(GfxRenderer& r, const Rect& area, const GameViewContext& ctx) {
  const auto& game = static_cast<const ConnectFour&>(ctx.game);
  const int lineH = r.getLineHeight(gameui::labelFont());

  // Portrait keeps the legend under the board; landscape gives it a side
  // column so the board can use the full height.
  const bool wide = area.width > area.height;
  const int legendW = wide ? std::min(area.width / 3, 220) : 0;
  const int legendH = wide ? 0 : lineH + 16;
  const int boardAreaW = area.width - legendW;
  const int boardAreaH = area.height - legendH;

  // Height budget: marker (half a cell) + frame + six rows + frame.
  const int cell = std::min({(boardAreaW - 2 * FRAME) / COLS, (boardAreaH - 2 * FRAME) * 2 / (ROWS * 2 + 1), 96});
  const int markerH = cell / 2;
  const int boardW = cell * COLS;
  const int boardH = cell * ROWS;
  const int stackH = markerH + boardH + 2 * FRAME;
  const int top = area.y + std::max(0, (boardAreaH - stackH) / 2);
  const int x0 = area.x + (boardAreaW - boardW) / 2;
  const int y0 = top + markerH + FRAME;
  const int d = cell * 4 / 5;
  const int inset = (cell - d) / 2;

  r.drawRoundedRect(x0 - FRAME, y0 - FRAME, boardW + 2 * FRAME, boardH + 2 * FRAME, 2, 10, true);

  for (int col = 0; col < COLS; col++) {
    for (int row = 0; row < ROWS; row++) {
      const int x = x0 + col * cell + inset;
      const int y = y0 + (ROWS - 1 - row) * cell + inset;
      const uint8_t piece = game.cell(col, row);
      drawPiece(r, x, y, d, piece);
      if (piece != 0 && game.isWinningCell(col, row)) {
        // Winning four: a contrasting core.
        const int core = d / 3;
        r.fillRoundedRect(x + (d - core) / 2, y + (d - core) / 2, core, core, core / 2,
                          piece == 1 ? Color::White : Color::Black);
      } else if (col == game.lastCol() && row == game.lastRow()) {
        // Last move: a ring around the cell.
        r.drawRoundedRect(x - inset + 1, y - inset + 1, cell - 2, cell - 2, 1, (cell - 2) / 2, true);
      }
    }
    // Each column (marker band plus board) is one tap target.
    ctx.targets.add(x0 + col * cell, top, cell, stackH, col);
  }

  if (ctx.localSeat >= 0 && !game.over()) {
    const int cx = x0 + ctx.cursor.a * cell + cell / 2;
    const int s = std::max(10, markerH - 4);
    const int my = top + (markerH - s * 2 / 3) / 2;
    if (ctx.myTurn) {
      gameui::drawDownMarker(r, cx, my, s);
    } else {
      // Off-turn the cursor stays visible but hollow: it is a preview only.
      r.drawLine(cx - s / 2, my, cx + s / 2, my, 2, true);
      r.drawLine(cx - s / 2, my, cx, my + s * 2 / 3, 2, true);
      r.drawLine(cx + s / 2, my, cx, my + s * 2 / 3, 2, true);
    }
  }

  // Legend: which piece belongs to whom, and whose move it is.
  if (wide) {
    const int lx = area.x + boardAreaW + 8;
    const int ly = area.y + (area.height - (2 * lineH + 16)) / 2;
    for (int seat = 0; seat < 2; seat++)
      drawLegendEntry(r, ctx, game, seat, lx, ly + seat * (lineH + 16), legendW - 16);
  } else {
    const int ly = y0 + boardH + FRAME + (legendH - lineH) / 2;
    const int half = area.width / 2;
    for (int seat = 0; seat < 2; seat++) drawLegendEntry(r, ctx, game, seat, area.x + seat * half + 8, ly, half - 16);
  }
}

const char* confirmLabel(const table::Game&, int, const GameCursor&) { return tr(STR_GAMES_DROP); }

}  // namespace

extern const GameView CONNECT_FOUR_VIEW = {
    table::GameId::ConnectFour, StrId::STR_GAME_CONNECT_FOUR, handleInput, handleTap, syncCursor, render, confirmLabel};

#include <LiarsDice.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "GameView.h"

using table::LiarsDice;

namespace {

// Cursor: a = focused control, b = bid quantity, c = bid face.
enum Focus : int8_t { FOCUS_QTY = 0, FOCUS_FACE = 1, FOCUS_BID = 2, FOCUS_LIAR = 3 };

// Tap target values.
enum Tap : int8_t { TAP_QTY_UP, TAP_QTY_DOWN, TAP_FACE_UP, TAP_FACE_DOWN, TAP_BID, TAP_LIAR, TAP_ROLL };

constexpr int CONTROLS_H = 64;

uint32_t signatureOf(const LiarsDice& game) {
  return (static_cast<uint32_t>(game.round()) << 16) | (static_cast<uint32_t>(game.bidQuantity()) << 8) |
         (static_cast<uint32_t>(game.bidFace()) << 4) | static_cast<uint32_t>(game.phase());
}

// The face the local cup supports best (ones count as wild), ties to higher.
uint8_t bestFace(const LiarsDice& game, const int seat) {
  if (seat < 0) return 2;
  uint8_t best = 2;
  int bestCount = -1;
  for (uint8_t f = 2; f <= 6; f++) {
    int count = 0;
    for (uint8_t i = 0; i < game.diceRolled(static_cast<uint8_t>(seat)); i++) {
      const uint8_t d = game.die(static_cast<uint8_t>(seat), i);
      if (d == f || d == 1) count++;
    }
    if (count >= bestCount) {
      bestCount = count;
      best = f;
    }
  }
  return best;
}

void syncCursor(const table::Game& g, const int seat, GameCursor& cursor) {
  const auto& game = static_cast<const LiarsDice&>(g);
  const uint32_t sig = signatureOf(game);
  if (cursor.signature != sig) {
    cursor.signature = sig;
    uint8_t face = bestFace(game, seat);
    uint8_t qty = game.minQuantityFor(face);
    for (uint8_t f = 6; qty == 0 && f >= 2; f--) {
      face = f;
      qty = game.minQuantityFor(f);
    }
    cursor.a = FOCUS_BID;
    cursor.b = static_cast<int8_t>(qty);
    cursor.c = static_cast<int8_t>(face);
  }
  if (!game.hasBid() && cursor.a == FOCUS_LIAR) cursor.a = FOCUS_BID;
}

void stepQuantity(const LiarsDice& game, GameCursor& cursor, const int delta) {
  const int minQ = game.minQuantityFor(static_cast<uint8_t>(cursor.c));
  cursor.b = static_cast<int8_t>(std::clamp<int>(cursor.b + delta, std::max(1, minQ), game.totalDice()));
}

void stepFace(const LiarsDice& game, GameCursor& cursor, const int delta) {
  int face = cursor.c;
  for (int step = 0; step < 5; step++) {
    face += delta;
    if (face > 6) face = 2;
    if (face < 2) face = 6;
    if (game.minQuantityFor(static_cast<uint8_t>(face)) != 0) break;
  }
  cursor.c = static_cast<int8_t>(face);
  cursor.b = static_cast<int8_t>(std::max<int>(cursor.b, game.minQuantityFor(static_cast<uint8_t>(face))));
}

bool commit(const LiarsDice& game, const GameCursor& cursor, uint8_t* action, size_t& actionLen) {
  if (cursor.a == FOCUS_LIAR) {
    if (!game.hasBid()) return false;
    actionLen = LiarsDice::encodeLiar(action);
    return true;
  }
  const auto qty = static_cast<uint8_t>(cursor.b);
  const auto face = static_cast<uint8_t>(cursor.c);
  if (!game.bidIsLegal(qty, face)) return false;
  actionLen = LiarsDice::encodeBid(action, qty, face);
  return true;
}

bool canRoll(const LiarsDice& game, const int seat) {
  return game.phase() == LiarsDice::Phase::Reveal && seat >= 0 && game.diceLeft(static_cast<uint8_t>(seat)) > 0;
}

bool handleInput(const MappedInputManager& input, const table::Game& g, const int seat, const bool myTurn,
                 GameCursor& cursor, uint8_t* action, size_t& actionLen) {
  const auto& game = static_cast<const LiarsDice&>(g);
  using Button = MappedInputManager::Button;

  if (game.phase() == LiarsDice::Phase::Reveal) {
    if (canRoll(game, seat) && input.wasReleased(Button::Confirm)) {
      actionLen = LiarsDice::encodeNextRound(action);
      return true;
    }
    return false;
  }
  if (game.phase() != LiarsDice::Phase::Bidding || !myTurn) return false;

  const int controls = game.hasBid() ? 4 : 3;
  if (input.wasPressed(Button::ScreenLeft)) {
    cursor.a = static_cast<int8_t>((cursor.a + controls - 1) % controls);
    return true;
  }
  if (input.wasPressed(Button::ScreenRight)) {
    cursor.a = static_cast<int8_t>((cursor.a + 1) % controls);
    return true;
  }
  const bool up = input.wasPressed(Button::ScreenUp);
  const bool down = input.wasPressed(Button::ScreenDown);
  if (up || down) {
    // Up/Down change the focused value; on Bid they change the quantity.
    if (cursor.a == FOCUS_FACE) {
      stepFace(game, cursor, up ? 1 : -1);
    } else if (cursor.a != FOCUS_LIAR) {
      stepQuantity(game, cursor, up ? 1 : -1);
    }
    return true;
  }
  if (input.wasReleased(Button::Confirm)) return commit(game, cursor, action, actionLen);
  return false;
}

bool handleTap(const table::Game& g, const int seat, const bool myTurn, const int value, GameCursor& cursor,
               uint8_t* action, size_t& actionLen) {
  const auto& game = static_cast<const LiarsDice&>(g);
  if (value == TAP_ROLL) {
    if (!canRoll(game, seat)) return false;
    actionLen = LiarsDice::encodeNextRound(action);
    return true;
  }
  if (game.phase() != LiarsDice::Phase::Bidding || !myTurn) return false;
  switch (value) {
    case TAP_QTY_UP:
    case TAP_QTY_DOWN:
      cursor.a = FOCUS_QTY;
      stepQuantity(game, cursor, value == TAP_QTY_UP ? 1 : -1);
      return true;
    case TAP_FACE_UP:
    case TAP_FACE_DOWN:
      cursor.a = FOCUS_FACE;
      stepFace(game, cursor, value == TAP_FACE_UP ? 1 : -1);
      return true;
    case TAP_BID:
      cursor.a = FOCUS_BID;
      commit(game, cursor, action, actionLen);
      return true;
    case TAP_LIAR:
      if (!game.hasBid()) return false;
      cursor.a = FOCUS_LIAR;
      commit(game, cursor, action, actionLen);
      return true;
    default:
      return false;
  }
}

const char* nameOf(const GameViewContext& ctx, const uint8_t seat) {
  return seat < ctx.game.seatCount() ? ctx.seatNames[seat] : "?";
}

// "<name> bid 4 × [die]" on one line.
void drawBid(GfxRenderer& r, const int x, const int y, const int maxWidth, const LiarsDice& game,
             const GameViewContext& ctx) {
  const int font = gameui::boldFont();
  const int lineH = r.getLineHeight(font);
  if (!game.hasBid()) {
    gameui::drawFittedText(r, font, x, y, tr(STR_LIARS_NO_BID), maxWidth);
    return;
  }
  const int dieSize = lineH + 4;
  char label[48];
  snprintf(label, sizeof(label), tr(STR_LIARS_BID_BY), nameOf(ctx, game.bidderSeat()));
  char amount[12];
  snprintf(amount, sizeof(amount), " %u ×", static_cast<unsigned>(game.bidQuantity()));
  const int amountW = r.getTextWidth(font, amount, EpdFontFamily::BOLD);
  const std::string fitted = r.truncatedText(font, label, maxWidth - amountW - dieSize - 8);
  r.drawText(font, x, y, fitted.c_str(), true);
  int cx = x + r.getTextWidth(font, fitted.c_str());
  r.drawText(font, cx, y, amount, true, EpdFontFamily::BOLD);
  cx += amountW + 6;
  gameui::drawDie(r, cx, y + (lineH - dieSize) / 2, dieSize, game.bidFace(), false);
}

void drawStepArrows(const GfxRenderer& r, const int x, const int y, const int w, const int h, const bool ink) {
  // Small up/down triangles hugging the top and bottom edges of a box.
  const int s = 7;
  const int cx = x + w / 2;
  const int upXs[3] = {cx - s, cx + s, cx};
  const int upYs[3] = {y + 4 + s, y + 4 + s, y + 4};
  const int downXs[3] = {cx - s, cx + s, cx};
  const int downYs[3] = {y + h - 4 - s, y + h - 4 - s, y + h - 4};
  r.fillPolygon(upXs, upYs, 3, ink);
  r.fillPolygon(downXs, downYs, 3, ink);
}

void drawControls(GfxRenderer& r, const Rect& box, const LiarsDice& game, const GameViewContext& ctx) {
  const GameCursor& cursor = ctx.cursor;
  const int font = gameui::boldFont();
  const int lineH = r.getLineHeight(font);
  const int count = game.hasBid() ? 4 : 3;
  const int gap = 8;
  const int w = (box.width - gap * (count - 1)) / count;
  for (int i = 0; i < count; i++) {
    const int x = box.x + i * (w + gap);
    const bool focused = cursor.a == i;
    if (focused) {
      r.fillRoundedRect(x, box.y, w, box.height, 8, Color::Black);
    } else {
      r.drawRoundedRect(x, box.y, w, box.height, 2, 8, true);
    }
    const int textY = box.y + (box.height - lineH) / 2;
    if (i == FOCUS_QTY || i == FOCUS_FACE) {
      // Upper half steps up, lower half steps down.
      const int upTap = i == FOCUS_QTY ? TAP_QTY_UP : TAP_FACE_UP;
      ctx.targets.add(x, box.y, w, box.height / 2, upTap);
      ctx.targets.add(x, box.y + box.height / 2, w, box.height - box.height / 2, upTap + 1);
      if (focused) drawStepArrows(r, x, box.y, w, box.height, false);
    } else {
      ctx.targets.add(x, box.y, w, box.height, i == FOCUS_BID ? TAP_BID : TAP_LIAR);
    }
    if (i == FOCUS_FACE) {
      const int dieSize = std::min(box.height - 26, w - 16);
      gameui::drawDie(r, x + (w - dieSize) / 2, box.y + (box.height - dieSize) / 2, dieSize,
                      static_cast<uint8_t>(cursor.c), false);
      continue;
    }
    char qty[8];
    const char* text = nullptr;
    if (i == FOCUS_QTY) {
      snprintf(qty, sizeof(qty), "%d ×", cursor.b);
      text = qty;
    } else {
      text = i == FOCUS_BID ? tr(STR_LIARS_BID) : tr(STR_LIARS_CALL);
    }
    const int tw = r.getTextWidth(font, text, EpdFontFamily::BOLD);
    r.drawText(font, x + std::max(4, (w - tw) / 2), textY, text, !focused, EpdFontFamily::BOLD);
  }
}

void drawPlayer(GfxRenderer& r, const GameViewContext& ctx, const LiarsDice& game, const uint8_t seat, const int x,
                const int y, const int width, const int rowH) {
  const int font = gameui::labelFont();
  const int lineH = r.getLineHeight(font);
  const int midY = y + rowH / 2;
  const int nameW = width * 9 / 25;
  const bool reveal = game.phase() != LiarsDice::Phase::Bidding;
  const LiarsDice::Result& result = game.lastResult();
  const bool mine = seat == ctx.localSeat;
  const bool winner = game.over() && (game.winnerMask() >> seat) & 1u;

  if (game.currentSeat() == seat) gameui::drawTurnMarker(r, x, midY, lineH / 2);
  const int nameX = x + lineH / 2 + 6;
  gameui::drawFittedText(r, font, nameX, midY - lineH / 2, nameOf(ctx, seat), x + nameW - nameX - 4, true,
                         mine || winner ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

  const int diceX = x + nameW;
  const uint8_t rolled = game.diceRolled(seat);
  if (rolled == 0 && game.diceLeft(seat) == 0) {
    r.drawText(font, diceX, midY - lineH / 2, tr(STR_LIARS_OUT), true);
    return;
  }
  // Your own cup is what you study all game: it gets the biggest dice.
  const int perDie = (width - nameW) / LiarsDice::START_DICE;
  const int dieSize = std::max(18, std::min({rowH - (mine ? 4 : 10), perDie - 6, mine ? 52 : 40}));
  for (uint8_t i = 0; i < rolled; i++) {
    const uint8_t face = game.die(seat, i);
    // In the reveal, dice that counted toward the challenged bid are framed.
    const bool counted = reveal && result.valid && face != 0 && (face == result.face || face == 1);
    gameui::drawDie(r, diceX + i * (dieSize + 6), midY - dieSize / 2, dieSize, face, counted);
  }
  // The die just lost is still shown in the reveal; strike it out.
  if (reveal && result.valid && result.loser == seat && game.diceLeft(seat) < rolled) {
    const int lx = diceX + (rolled - 1) * (dieSize + 6);
    r.drawLine(lx - 2, midY + dieSize / 2 + 1, lx + dieSize + 2, midY - dieSize / 2 - 1, 3, true);
  }
}

void render(GfxRenderer& r, const Rect& area, const GameViewContext& ctx) {
  const auto& game = static_cast<const LiarsDice&>(ctx.game);
  const int font = gameui::labelFont();
  const int lineH = r.getLineHeight(font);
  const int boldH = r.getLineHeight(gameui::boldFont());
  const LiarsDice::Result& result = game.lastResult();
  const bool showControls = ctx.myTurn && game.phase() == LiarsDice::Phase::Bidding;
  const bool rollHint = canRoll(game, ctx.localSeat);
  const int seats = game.seatCount();
  const int x = area.x + 8;
  const int width = area.width - 16;

  // Fixed blocks: info and bid on top; result and controls (or the roll
  // hint) anchored at the bottom. Player rows share what is left, in two
  // columns when one would squeeze them.
  const int topH = lineH + 8 + boldH + 14;
  const int footerH = showControls ? CONTROLS_H + 8 : (rollHint ? lineH + 14 : 0);
  const int resultH = result.valid ? 2 * lineH + 12 : 0;
  const int avail = area.height - topH - footerH - resultH;
  const int columns = avail / std::max(1, seats) < 34 && seats > 1 ? 2 : 1;
  const int rowsPerColumn = (seats + columns - 1) / columns;
  const int rowH = std::clamp(avail / std::max(1, rowsPerColumn), 26, 64);
  const int colW = width / columns;

  int y = area.y + 4;
  char line[96];
  snprintf(line, sizeof(line), tr(STR_LIARS_ROUND), game.round());
  std::string info = line;
  info += "  ·  ";
  info += tr(STR_LIARS_ONES_WILD);
  gameui::drawFittedText(r, font, x, y, info.c_str(), width);
  y += lineH + 8;
  drawBid(r, x, y, width, game, ctx);
  y += boldH + 14;

  for (int s = 0; s < seats; s++) {
    const int col = s / rowsPerColumn;
    const int row = s % rowsPerColumn;
    drawPlayer(r, ctx, game, static_cast<uint8_t>(s), x + col * colW, y + row * rowH, colW - 8, rowH);
  }
  y += rowsPerColumn * rowH;

  // The result sits under the table, never below the footer's top edge.
  const int footerTop = area.y + area.height - footerH;
  if (result.valid) {
    y = std::min(y + 6, footerTop - resultH + 6);
    snprintf(line, sizeof(line), tr(STR_LIARS_CALLED), nameOf(ctx, result.challenger), nameOf(ctx, result.bidder));
    gameui::drawFittedText(r, font, x, y, line, width);
    y += lineH + 2;
    snprintf(line, sizeof(line), tr(STR_LIARS_THERE_WERE), result.actual, nameOf(ctx, result.loser));
    gameui::drawFittedText(r, font, x, y, line, width, true, EpdFontFamily::BOLD);
  }

  if (showControls) {
    drawControls(r, Rect{x, footerTop + 4, width, CONTROLS_H}, game, ctx);
  } else if (rollHint) {
    r.drawCenteredText(font, footerTop + 6, tr(STR_LIARS_ROLL_HINT), true);
    // Anywhere on the table rolls on touch boards.
    ctx.targets.add(area.x, area.y, area.width, area.height, TAP_ROLL);
  }
}

const char* confirmLabel(const table::Game& g, int, const GameCursor& cursor) {
  const auto& game = static_cast<const LiarsDice&>(g);
  if (game.phase() == LiarsDice::Phase::Reveal) return tr(STR_LIARS_ROLL);
  return cursor.a == FOCUS_LIAR ? tr(STR_LIARS_CALL) : tr(STR_LIARS_BID);
}

}  // namespace

extern const GameView LIARS_DICE_VIEW = {
    table::GameId::LiarsDice, StrId::STR_GAME_LIARS_DICE, handleInput, handleTap, syncCursor, render, confirmLabel};

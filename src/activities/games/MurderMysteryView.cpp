#include <MurderMystery.h>

#include <algorithm>
#include <cstdio>

#include "GameView.h"

using table::MurderMystery;

namespace {

// Cursor while naming a theory: a = focus (a category column, then the
// Suggest and Accuse buttons), b/c/d = chosen suspect/weapon/room index.
// While showing a card, a = which of the matching cards.
enum Focus : int8_t { FOCUS_SUSPECT = 0, FOCUS_WEAPON = 1, FOCUS_ROOM = 2, FOCUS_SUGGEST = 3, FOCUS_ACCUSE = 4 };
constexpr int FOCUS_COUNT = 5;

// Tap target values; cards tap as their id (0-17).
enum Tap : int16_t { TAP_SUGGEST = 32, TAP_ACCUSE = 33, TAP_CONTINUE = 34 };

constexpr int CONTROLS_H = 56;

constexpr StrId CATEGORY_NAMES[MurderMystery::CATEGORIES] = {StrId::STR_MYSTERY_SUSPECT, StrId::STR_MYSTERY_WEAPON,
                                                             StrId::STR_MYSTERY_ROOM};
constexpr StrId CARD_NAMES[MurderMystery::CARD_COUNT] = {
    StrId::STR_MYSTERY_GRAVES,      StrId::STR_MYSTERY_ASHFORD, StrId::STR_MYSTERY_CRANE,
    StrId::STR_MYSTERY_ROOK,        StrId::STR_MYSTERY_VALE,    StrId::STR_MYSTERY_QUILL,
    StrId::STR_MYSTERY_CANDLESTICK, StrId::STR_MYSTERY_DAGGER,  StrId::STR_MYSTERY_ROPE,
    StrId::STR_MYSTERY_REVOLVER,    StrId::STR_MYSTERY_POISON,  StrId::STR_MYSTERY_WRENCH,
    StrId::STR_MYSTERY_LIBRARY,     StrId::STR_MYSTERY_STUDY,   StrId::STR_MYSTERY_KITCHEN,
    StrId::STR_MYSTERY_BALLROOM,    StrId::STR_MYSTERY_CELLAR,  StrId::STR_MYSTERY_GARDEN,
};

const char* cardName(const uint8_t card) { return card < MurderMystery::CARD_COUNT ? I18N.get(CARD_NAMES[card]) : "?"; }

const char* nameOf(const GameViewContext& ctx, const uint8_t seat) {
  return seat < ctx.game.seatCount() ? ctx.seatNames[seat] : "?";
}

int8_t& pickOf(GameCursor& cursor, const int category) {
  return category == 0 ? cursor.b : (category == 1 ? cursor.c : cursor.d);
}

int8_t pickOf(const GameCursor& cursor, const int category) {
  return category == 0 ? cursor.b : (category == 1 ? cursor.c : cursor.d);
}

// Cards the local player knows are innocent: their hand, cards shown to
// them, and the hands of players who left.
uint32_t knownMask(const MurderMystery& game, const int seat) {
  const uint32_t own =
      seat >= 0 ? game.handOf(static_cast<uint8_t>(seat)) | game.seenBy(static_cast<uint8_t>(seat)) : 0;
  return own | game.revealedCards();
}

uint32_t signatureOf(const MurderMystery& game) {
  const uint8_t latest = game.eventCount() > 0 ? game.event(0).seat : 0x0F;
  return static_cast<uint32_t>(game.phase()) | (static_cast<uint32_t>(game.turnSeat()) << 4) |
         (static_cast<uint32_t>(game.refuterSeat()) << 8) | (static_cast<uint32_t>(game.eventCount()) << 16) |
         (static_cast<uint32_t>(latest & 0x0F) << 20);
}

void syncCursor(const table::Game& g, const int seat, GameCursor& cursor) {
  const auto& game = static_cast<const MurderMystery&>(g);
  const uint32_t sig = signatureOf(game);
  if (cursor.signature == sig) return;
  cursor.signature = sig;
  cursor.a = 0;
  if (game.phase() != MurderMystery::Phase::Turn) return;
  // Start each column on the first card not yet ruled out.
  const uint32_t known = knownMask(game, seat);
  for (uint8_t c = 0; c < MurderMystery::CATEGORIES; c++) {
    int8_t& pick = pickOf(cursor, c);
    pick = 0;
    for (uint8_t i = 0; i < MurderMystery::PER_CATEGORY; i++) {
      if (!(known & MurderMystery::cardBit(MurderMystery::cardOf(c, i)))) {
        pick = static_cast<int8_t>(i);
        break;
      }
    }
  }
}

size_t commitTheory(const GameCursor& cursor, const bool accuse, uint8_t* action) {
  const auto s = static_cast<uint8_t>(cursor.b);
  const auto w = static_cast<uint8_t>(cursor.c);
  const auto r = static_cast<uint8_t>(cursor.d);
  return accuse ? MurderMystery::encodeAccuse(action, s, w, r) : MurderMystery::encodeSuggest(action, s, w, r);
}

bool handleInput(const MappedInputManager& input, const table::Game& g, const int seat, const bool myTurn,
                 GameCursor& cursor, uint8_t* action, size_t& actionLen) {
  const auto& game = static_cast<const MurderMystery&>(g);
  using Button = MappedInputManager::Button;
  if (!myTurn) return false;
  const bool left = input.wasPressed(Button::ScreenLeft);
  const bool right = input.wasPressed(Button::ScreenRight);
  const bool up = input.wasPressed(Button::ScreenUp);
  const bool down = input.wasPressed(Button::ScreenDown);

  switch (game.phase()) {
    case MurderMystery::Phase::Turn: {
      if (left || right) {
        cursor.a = static_cast<int8_t>((cursor.a + (right ? 1 : FOCUS_COUNT - 1)) % FOCUS_COUNT);
        return true;
      }
      if (up || down) {
        if (cursor.a < FOCUS_SUGGEST) {
          int8_t& pick = pickOf(cursor, cursor.a);
          pick =
              static_cast<int8_t>((pick + (down ? 1 : MurderMystery::PER_CATEGORY - 1)) % MurderMystery::PER_CATEGORY);
        } else {
          cursor.a = cursor.a == FOCUS_SUGGEST ? FOCUS_ACCUSE : FOCUS_SUGGEST;
        }
        return true;
      }
      if (input.wasReleased(Button::Confirm)) {
        // Confirm walks suspect -> weapon -> room -> Suggest before it commits.
        if (cursor.a < FOCUS_SUGGEST) {
          cursor.a++;
          return true;
        }
        actionLen = commitTheory(cursor, cursor.a == FOCUS_ACCUSE, action);
        return true;
      }
      return false;
    }
    case MurderMystery::Phase::Refute: {
      uint8_t options[MurderMystery::CATEGORIES];
      const uint8_t n = game.refuteOptions(static_cast<uint8_t>(seat), options);
      if (n == 0) return false;
      if (left || right || up || down) {
        cursor.a = static_cast<int8_t>((cursor.a + (right || down ? 1 : n - 1)) % n);
        return true;
      }
      if (input.wasReleased(Button::Confirm)) {
        actionLen = MurderMystery::encodeShow(action, options[std::clamp<int>(cursor.a, 0, n - 1)]);
        return true;
      }
      return false;
    }
    case MurderMystery::Phase::Result:
      if (input.wasReleased(Button::Confirm)) {
        actionLen = MurderMystery::encodeContinue(action);
        return true;
      }
      return false;
    case MurderMystery::Phase::Over:
      break;
  }
  return false;
}

bool handleTap(const table::Game& g, const int seat, const bool myTurn, const int value, GameCursor& cursor,
               uint8_t* action, size_t& actionLen) {
  const auto& game = static_cast<const MurderMystery&>(g);
  if (!myTurn) return false;
  switch (game.phase()) {
    case MurderMystery::Phase::Turn:
      if (value >= 0 && value < MurderMystery::CARD_COUNT) {
        const auto card = static_cast<uint8_t>(value);
        cursor.a = static_cast<int8_t>(MurderMystery::categoryOf(card));
        pickOf(cursor, cursor.a) = static_cast<int8_t>(MurderMystery::indexOf(card));
        return true;
      }
      if (value == TAP_SUGGEST || value == TAP_ACCUSE) {
        cursor.a = value == TAP_SUGGEST ? FOCUS_SUGGEST : FOCUS_ACCUSE;
        actionLen = commitTheory(cursor, value == TAP_ACCUSE, action);
        return true;
      }
      return false;
    case MurderMystery::Phase::Refute: {
      uint8_t options[MurderMystery::CATEGORIES];
      const uint8_t n = game.refuteOptions(static_cast<uint8_t>(seat), options);
      for (uint8_t i = 0; i < n; i++) {
        if (options[i] != value) continue;
        cursor.a = static_cast<int8_t>(i);
        actionLen = MurderMystery::encodeShow(action, options[i]);
        return true;
      }
      return false;
    }
    case MurderMystery::Phase::Result:
      if (value != TAP_CONTINUE) return false;
      actionLen = MurderMystery::encodeContinue(action);
      return true;
    case MurderMystery::Phase::Over:
      break;
  }
  return false;
}

void formatTheory(char* out, const size_t size, const uint8_t* cards) {
  snprintf(out, size, tr(STR_MYSTERY_THEORY), cardName(cards[0]), cardName(cards[1]), cardName(cards[2]));
}

// The three lines at the top of the screen.
struct Summary {
  char headline[40] = {};
  char theory[80] = {};
  char outcome[72] = {};
};

// Describes the newest event. Returns false when there is none yet.
bool describeLatest(const MurderMystery& game, const GameViewContext& ctx, Summary& out) {
  out = Summary{};
  if (game.eventCount() == 0) return false;
  const MurderMystery::Event& e = game.event(0);
  const bool accusation = e.kind != MurderMystery::EventKind::Suggestion;
  snprintf(out.headline, sizeof(out.headline),
           accusation ? tr(STR_MYSTERY_ACCUSATION_BY) : tr(STR_MYSTERY_SUGGESTION_BY), nameOf(ctx, e.seat));
  formatTheory(out.theory, sizeof(out.theory), e.cards);
  char* outcome = out.outcome;
  constexpr size_t size = sizeof(out.outcome);
  // The exchange is still on screen until its player continues.
  const bool live = game.phase() == MurderMystery::Phase::Refute || game.phase() == MurderMystery::Phase::Result;
  switch (e.kind) {
    case MurderMystery::EventKind::WrongAccusation:
      snprintf(outcome, size, "%s", tr(STR_MYSTERY_WRONG));
      break;
    case MurderMystery::EventKind::RightAccusation:
      snprintf(outcome, size, "%s", tr(STR_MYSTERY_SOLVED));
      break;
    case MurderMystery::EventKind::Suggestion:
      if (live && game.phase() == MurderMystery::Phase::Refute) {
        if (e.refuter == ctx.localSeat) {
          snprintf(outcome, size, tr(STR_MYSTERY_SHOW_PROMPT), nameOf(ctx, e.seat));
        } else {
          snprintf(outcome, size, tr(STR_MYSTERY_WAITING_SHOW), nameOf(ctx, e.refuter));
        }
      } else if (e.refuter == MurderMystery::NO_SEAT) {
        snprintf(outcome, size, "%s", tr(STR_MYSTERY_NOBODY));
      } else if (live && game.shownCard() != MurderMystery::NO_CARD && e.seat == ctx.localSeat) {
        snprintf(outcome, size, tr(STR_MYSTERY_SHOWN_TO_YOU), nameOf(ctx, e.refuter), cardName(game.shownCard()));
      } else if (live && game.shownCard() != MurderMystery::NO_CARD && e.refuter == ctx.localSeat) {
        snprintf(outcome, size, tr(STR_MYSTERY_YOU_SHOWED), cardName(game.shownCard()));
      } else {
        snprintf(outcome, size, tr(STR_MYSTERY_SHOWED_CARD), nameOf(ctx, e.refuter));
      }
      break;
  }
  return true;
}

// One notebook entry: a box (filled for your own cards, crossed for cards
// ruled out) and the name, struck through once it is ruled out.
void drawCard(GfxRenderer& r, const MurderMystery& game, const GameViewContext& ctx, const uint8_t card, const int x,
              const int y, const int w, const int h, const bool inverted, const bool framed) {
  const int font = gameui::labelFont();
  const int lineH = r.getLineHeight(font);
  const bool ink = !inverted;
  const int seat = ctx.localSeat;
  const bool mine = seat >= 0 && (game.handOf(static_cast<uint8_t>(seat)) & MurderMystery::cardBit(card));
  const bool ruledOut = knownMask(game, seat) & MurderMystery::cardBit(card);
  const bool guilty = game.solutionCard(MurderMystery::categoryOf(card)) == card;

  if (inverted) r.fillRoundedRect(x, y, w, h, 6, Color::Black);
  if (framed || guilty) r.drawRoundedRect(x, y, w, h, guilty ? 3 : 2, 6, true);

  const int box = std::max(10, lineH * 3 / 5);
  const int boxX = x + 6;
  const int boxY = y + (h - box) / 2;
  if (mine) {
    r.fillRect(boxX, boxY, box, box, ink);
  } else {
    r.drawRect(boxX, boxY, box, box, 2, ink);
    if (ruledOut) {
      r.drawLine(boxX + 2, boxY + 2, boxX + box - 3, boxY + box - 3, 2, ink);
      r.drawLine(boxX + 2, boxY + box - 3, boxX + box - 3, boxY + 2, 2, ink);
    }
  }
  const int textX = boxX + box + 6;
  const int textY = y + (h - lineH) / 2;
  const int maxW = x + w - textX - 4;
  const auto style = guilty ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  gameui::drawFittedText(r, font, textX, textY, cardName(card), maxW, ink, style);
  if (ruledOut) {
    const int textW = std::min(maxW, r.getTextWidth(font, cardName(card), style));
    r.drawLine(textX, y + h / 2, textX + textW, y + h / 2, ink);
  }
}

void drawNotebook(GfxRenderer& r, const Rect& box, const int headerH, const int rowH, const MurderMystery& game,
                  const GameViewContext& ctx) {
  const GameCursor& cursor = ctx.cursor;
  const int font = gameui::labelFont();
  const int colW = box.width / MurderMystery::CATEGORIES;
  const bool choosing = ctx.myTurn && game.phase() == MurderMystery::Phase::Turn;
  const bool refuting = ctx.myTurn && game.phase() == MurderMystery::Phase::Refute;
  uint8_t options[MurderMystery::CATEGORIES] = {};
  const uint8_t optionCount = refuting ? game.refuteOptions(static_cast<uint8_t>(ctx.localSeat), options) : 0;
  const bool exchange = game.phase() == MurderMystery::Phase::Refute || game.phase() == MurderMystery::Phase::Result;

  for (uint8_t c = 0; c < MurderMystery::CATEGORIES; c++) {
    const int x = box.x + c * colW;
    const bool focused = choosing && cursor.a == c;
    gameui::drawFittedText(r, font, x + 6, box.y, I18N.get(CATEGORY_NAMES[c]), colW - 12, true, EpdFontFamily::BOLD);
    if (focused) r.fillRect(x + 6, box.y + headerH - 5, colW - 12, 3, true);
    for (uint8_t i = 0; i < MurderMystery::PER_CATEGORY; i++) {
      const uint8_t card = MurderMystery::cardOf(c, i);
      const int y = box.y + headerH + i * rowH;
      bool inverted = false;
      bool framed = false;
      if (choosing) {
        const bool picked = pickOf(cursor, c) == i;
        inverted = picked && (focused || cursor.a >= FOCUS_SUGGEST);
        framed = picked;
      } else if (refuting) {
        for (uint8_t o = 0; o < optionCount; o++) {
          if (options[o] != card) continue;
          framed = true;
          inverted = cursor.a == o;
        }
      } else if (exchange) {
        framed = game.pendingCard(c) == card;
      }
      drawCard(r, game, ctx, card, x + 2, y + 1, colW - 4, rowH - 2, inverted, framed);
      ctx.targets.add(x, y, colW, rowH, card);
    }
  }
}

// Earlier events, newest first: who asked, what, and who answered.
void drawCasebook(GfxRenderer& r, const Rect& box, const MurderMystery& game, const GameViewContext& ctx,
                  const uint8_t first) {
  const int font = gameui::labelFont();
  const int lineH = r.getLineHeight(font);
  const int rowH = lineH + 4;
  if (first >= game.eventCount() || box.height < 2 * rowH) return;
  const int answerW = box.width * 3 / 10;
  const int answerX = box.x + box.width - answerW;
  r.drawLine(box.x, box.y, box.x + box.width, box.y, true);
  int y = box.y + 4;
  gameui::drawFittedText(r, font, box.x, y, tr(STR_MYSTERY_CASEBOOK), answerX - box.x - 8, true, EpdFontFamily::BOLD);
  gameui::drawFittedText(r, font, answerX, y, tr(STR_MYSTERY_SHOWN_BY), answerW, true, EpdFontFamily::BOLD);
  y += rowH;
  char line[96];
  for (uint8_t i = first; i < game.eventCount() && y + rowH <= box.y + box.height; i++) {
    const MurderMystery::Event& e = game.event(i);
    snprintf(line, sizeof(line), "%s: %s, %s, %s", nameOf(ctx, e.seat), cardName(e.cards[0]), cardName(e.cards[1]),
             cardName(e.cards[2]));
    gameui::drawFittedText(r, font, box.x, y, line, answerX - box.x - 8);
    const char* answer = tr(STR_MYSTERY_NONE);
    if (e.kind == MurderMystery::EventKind::WrongAccusation) {
      answer = tr(STR_MYSTERY_WRONG_SHORT);
    } else if (e.kind == MurderMystery::EventKind::RightAccusation) {
      answer = tr(STR_MYSTERY_SOLVED_SHORT);
    } else if (e.refuter != MurderMystery::NO_SEAT) {
      answer = nameOf(ctx, e.refuter);
    }
    gameui::drawFittedText(
        r, font, answerX, y, answer, answerW, true,
        e.kind == MurderMystery::EventKind::Suggestion ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD);
    y += rowH;
  }
}

void drawButton(GfxRenderer& r, const int x, const int y, const int w, const int h, const char* label,
                const bool focused) {
  const int font = gameui::boldFont();
  if (focused) {
    r.fillRoundedRect(x, y, w, h, 8, Color::Black);
  } else {
    r.drawRoundedRect(x, y, w, h, 2, 8, true);
  }
  const int tw = r.getTextWidth(font, label, EpdFontFamily::BOLD);
  r.drawText(font, x + std::max(4, (w - tw) / 2), y + (h - r.getLineHeight(font)) / 2, label, !focused,
             EpdFontFamily::BOLD);
}

void render(GfxRenderer& r, const Rect& area, const GameViewContext& ctx) {
  const auto& game = static_cast<const MurderMystery&>(ctx.game);
  const int font = gameui::labelFont();
  const int bold = gameui::boldFont();
  const int lineH = r.getLineHeight(font);
  const int boldH = r.getLineHeight(bold);
  const int x = area.x + 8;
  const int width = area.width - 16;
  const bool choosing = ctx.myTurn && game.phase() == MurderMystery::Phase::Turn;
  const bool continuing = ctx.myTurn && game.phase() == MurderMystery::Phase::Result;

  // Top: what just happened (or what to do). Bottom: the buttons. The
  // notebook takes what it needs in between and the casebook the rest.
  Summary top;
  bool latestOnTop = false;
  if (game.over()) {
    describeLatest(game, ctx, top);
    snprintf(top.outcome, sizeof(top.outcome), tr(STR_MYSTERY_SOLUTION), cardName(game.solutionCard(0)),
             cardName(game.solutionCard(1)), cardName(game.solutionCard(2)));
    latestOnTop = true;
  } else if (choosing) {
    snprintf(top.headline, sizeof(top.headline), "%s", tr(STR_MYSTERY_PROMPT));
  } else {
    latestOnTop = describeLatest(game, ctx, top);
  }

  int y = area.y + 2;
  gameui::drawFittedText(r, bold, x, y, top.headline, width, true, EpdFontFamily::BOLD);
  y += boldH + 2;
  gameui::drawFittedText(r, font, x, y, top.theory, width);
  y += lineH + 2;
  gameui::drawFittedText(r, bold, x, y, top.outcome, width, true, EpdFontFamily::BOLD);
  y += boldH + 8;

  const int footerH = choosing || continuing ? CONTROLS_H + 8 : 0;
  const int footerTop = area.y + area.height - footerH;
  const int headerH = lineH + 6;
  const int rowH = std::clamp((footerTop - y - headerH) / MurderMystery::PER_CATEGORY, 22, 36);
  drawNotebook(r, Rect{x, y, width, headerH + rowH * MurderMystery::PER_CATEGORY}, headerH, rowH, game, ctx);
  y += headerH + rowH * MurderMystery::PER_CATEGORY + 8;

  drawCasebook(r, Rect{x, y, width, footerTop - y}, game, ctx, latestOnTop ? 1 : 0);

  if (choosing) {
    const int gap = 12;
    const int w = (width - gap) / 2;
    drawButton(r, x, footerTop + 4, w, CONTROLS_H, tr(STR_MYSTERY_SUGGEST), ctx.cursor.a == FOCUS_SUGGEST);
    drawButton(r, x + w + gap, footerTop + 4, w, CONTROLS_H, tr(STR_MYSTERY_ACCUSE), ctx.cursor.a == FOCUS_ACCUSE);
    ctx.targets.add(x, footerTop + 4, w, CONTROLS_H, TAP_SUGGEST);
    ctx.targets.add(x + w + gap, footerTop + 4, w, CONTROLS_H, TAP_ACCUSE);
  } else if (continuing) {
    drawButton(r, x, footerTop + 4, width, CONTROLS_H, tr(STR_MYSTERY_CONTINUE), true);
    ctx.targets.add(x, footerTop + 4, width, CONTROLS_H, TAP_CONTINUE);
  }
}

const char* confirmLabel(const table::Game& g, const int seat, const GameCursor& cursor) {
  const auto& game = static_cast<const MurderMystery&>(g);
  switch (game.phase()) {
    case MurderMystery::Phase::Turn:
      if (cursor.a == FOCUS_ACCUSE) return tr(STR_MYSTERY_ACCUSE);
      return cursor.a == FOCUS_SUGGEST ? tr(STR_MYSTERY_SUGGEST) : tr(STR_MYSTERY_NEXT);
    case MurderMystery::Phase::Refute:
      return tr(STR_MYSTERY_SHOW);
    case MurderMystery::Phase::Result:
      // Only the player whose turn it was moves the table on.
      return seat == game.turnSeat() ? tr(STR_MYSTERY_CONTINUE) : "";
    case MurderMystery::Phase::Over:
      break;
  }
  return "";
}

}  // namespace

extern const GameView MURDER_MYSTERY_VIEW = {table::GameId::MurderMystery,
                                             StrId::STR_GAME_MURDER_MYSTERY,
                                             handleInput,
                                             handleTap,
                                             syncCursor,
                                             render,
                                             confirmLabel};

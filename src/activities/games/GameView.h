#pragma once

#include <GfxRenderer.h>
#include <I18n.h>
#include <TableGame.h>

#include <cstddef>
#include <cstdint>

#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"

// Screen-side half of a table game: cursor handling on the loop task and
// drawing on the render task. Rules stay in lib/TableGames; a view only ever
// sees the game as the local player may (TableSession::view()).

// Per-game cursor state. Plain bytes so the activity can copy it to the
// render task alongside the serialized view.
struct GameCursor {
  int8_t a = 0;
  int8_t b = 0;
  int8_t c = 0;
  int8_t d = 0;
  // Views re-seed their defaults whenever this stops matching the state.
  uint32_t signature = 0xFFFFFFFFu;
};

// Tap targets a view registers while drawing; the activity hit-tests taps
// against the last published frame, so touch always matches the screen.
struct GameTargets {
  struct Target {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    int16_t value;
  };
  // Dots & Boxes registers one target per open line (60 on its board).
  static constexpr int MAX = 64;
  Target items[MAX] = {};
  uint8_t count = 0;

  void add(int x, int y, int w, int h, int value);
  // Value of the topmost target containing (x, y), or -1.
  int hit(int x, int y) const;
};

struct GameViewContext {
  const table::Game& game;
  // Game seat of this device, or -1 when watching.
  int localSeat;
  // Display name per game seat ("You" for the local player).
  const char* const* seatNames;
  const GameCursor& cursor;
  bool myTurn;
  // Where the view records its tap targets (never null).
  GameTargets& targets;
};

struct GameView {
  table::GameId id;
  StrId name;
  // Loop task. Moves the cursor and, when Confirm commits a move, writes it
  // to `action` (up to table::MAX_ACTION bytes) and sets actionLen. Returns
  // true when the screen should repaint.
  bool (*handleInput)(const MappedInputManager& input, const table::Game& game, int localSeat, bool myTurn,
                      GameCursor& cursor, uint8_t* action, size_t& actionLen);
  // Loop task. A tap landed on a target the view registered with `value`.
  // Same contract as handleInput.
  bool (*handleTap)(const table::Game& game, int localSeat, bool myTurn, int value, GameCursor& cursor, uint8_t* action,
                    size_t& actionLen);
  // Loop task. Re-seeds cursor defaults after the state moves on.
  void (*syncCursor)(const table::Game& game, int localSeat, GameCursor& cursor);
  // Render task. Draws the board into `area`.
  void (*render)(GfxRenderer& renderer, const Rect& area, const GameViewContext& ctx);
  // Label for the Confirm button hint while it is this player's move.
  const char* (*confirmLabel)(const table::Game& game, int localSeat, const GameCursor& cursor);
};

const GameView* gameViewFor(table::GameId id);
const char* gameName(table::GameId id);

namespace gameui {

// Font for board labels and scores.
int labelFont();
int boldFont();
// A solid (Black) or hollow disc of diameter d with its top-left at (x, y).
void drawDisc(const GfxRenderer& r, int x, int y, int d, bool filled);
// A die showing `face` (1-6), or a blank "hidden" die for face 0.
void drawDie(const GfxRenderer& r, int x, int y, int size, uint8_t face, bool highlighted);
// Small filled triangle pointing right, for turn markers.
void drawTurnMarker(const GfxRenderer& r, int x, int centerY, int size);
// Small filled triangle pointing down, for column cursors.
void drawDownMarker(const GfxRenderer& r, int centerX, int y, int size);
// Text truncated to fit maxWidth.
void drawFittedText(const GfxRenderer& r, int fontId, int x, int y, const char* text, int maxWidth, bool black = true,
                    EpdFontFamily::Style style = EpdFontFamily::REGULAR);

}  // namespace gameui

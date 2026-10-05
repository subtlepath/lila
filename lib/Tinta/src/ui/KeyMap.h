#pragma once

// Semantic input (PLAN.md §4.4). KeyMap turns the board's raw input events
// into the keys, taps and swipes the UI reasons about, owns the per-device
// table of where the four front keys sit under the screen, and decides what
// a key means on the current screen:
//
// - A screen that declares a choice bar gets its front keys as answers: the
//   key under footer cell i is ActionChoice(i), with no focus step. A key
//   under an empty cell falls through to the rules below.
// - Otherwise keys go through FreeInkUI focus routing: Confirm activates the
//   focused element, the side keys and Left/Right move focus, Back goes back.
// - Touch always goes through FreeInkUI interactions.
//
// App-level meanings (hold Back or tap Home for the pause sheet, hold Home
// for Home, Power for sleep, swipe down from the top for the light) are
// applied by app::App before any of this.

#include <FreeInkUICore.h>
#include <stdint.h>

#include "platform/Board.h"

namespace freeink {
struct Icon;
}

namespace tinta::ui {

enum class Key : uint8_t {
  Back,  // front keys (key devices)
  Confirm,
  Left,
  Right,
  Up,  // side keys (every device)
  Down,
  Power,
  Home,  // X4 Pro capacitive Home pad
};

// Lower-case name used in logs and tests: back, confirm, ..., home.
const char* keyName(Key key);

struct InputEvent {
  enum class Kind : uint8_t { Key, Tap, Swipe };
  Kind kind = Kind::Key;
  Key key = Key::Back;
  bool hold = false;  // Back held for platform::kBackHoldMs, or the Home pad held
  int16_t x = 0;      // Tap point or swipe start, logical portrait coordinates
  int16_t y = 0;
  int16_t x2 = 0;  // swipe end
  int16_t y2 = 0;
  freeink::ui::SwipeDir swipe = freeink::ui::SwipeDir::None;
  bool fromTopEdge = false;  // a swipe down that started in the top band
};

constexpr uint8_t kFooterCellCount = 4;

// One footer cell, in logical portrait pixels. On key devices each cell sits
// above a front key; on the X4 Pro the cells are touch targets only.
struct FooterCell {
  int16_t x;
  int16_t width;
  bool hasKey;
  Key key;
};

// What a footer cell says: a label, an optional second line (an interval, a
// date) and an optional icon. A cell with neither label nor icon is empty.
struct CellSpec {
  const char* label = nullptr;
  const char* detail = nullptr;
  const freeink::Icon* icon = nullptr;
  bool enabled = true;

  bool empty() const { return label == nullptr && icon == nullptr; }
};

// Up to four answers, one per footer cell.
struct ChoiceBar {
  CellSpec cells[kFooterCellCount];
};

class KeyMap {
 public:
  // `device` must describe the logical (portrait) frame the UI draws in.
  void begin(platform::Board& board, const freeink::ui::DeviceContext& device);

  // Next event, oldest first; false when none is pending.
  bool next(InputEvent& out);

  const FooterCell& footerCell(uint8_t index) const { return cells_[index < kFooterCellCount ? index : 0]; }

  // Index of the footer cell above `key`, or -1 when it is not a front key here.
  int8_t footerIndexOf(Key key) const;

  // The answer a key press or tap gives on a screen with `bar`, or -1 when it
  // is not one (an empty or disabled cell, a hold, a side key, a tap elsewhere).
  int8_t choiceFor(const InputEvent& event, const ChoiceBar& bar, int16_t barTop) const;

  // FreeInkUI's view of a key, tap or swipe for focus routing.
  freeink::ui::InputSnapshot snapshotFor(const InputEvent& event) const;

  // "back confirm left right": the front keys under cells 0..3, for logs.
  void describeOrder(char* out, size_t cap) const;

 private:
  platform::Board* board_ = nullptr;
  freeink::ui::DeviceContext device_{};
  FooterCell cells_[kFooterCellCount] = {};
};

}  // namespace tinta::ui

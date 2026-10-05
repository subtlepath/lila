#include "ui/KeyMap.h"

#include <stdio.h>

#include "platform/Log.h"

namespace tinta::ui {
namespace {

using freeink::ui::Point;

// Physical left-to-right order of the four front keys, and the footer cells
// above them as left edges in logical portrait pixels (the fifth entry is the
// right edge of the last cell).
//
// UNCONFIRMED: the real order and spacing are recorded on each device in the M0
// hardware check (docs/hardware-notes.md). Until then every key device assumes
// Back, Confirm, Left, Right under four equal cells. Edit the rows here.
struct FrontKeys {
  bool present;
  Key order[kFooterCellCount];
  int16_t edges[kFooterCellCount + 1];
};

constexpr FrontKeys kFrontKeys[platform::kDeviceCount] = {
    // X3, 528 px wide
    {true, {Key::Back, Key::Confirm, Key::Left, Key::Right}, {0, 132, 264, 396, 528}},
    // X4, 480 px wide
    {true, {Key::Back, Key::Confirm, Key::Left, Key::Right}, {0, 120, 240, 360, 480}},
    // X4 Classic, 480 px wide
    {true, {Key::Back, Key::Confirm, Key::Left, Key::Right}, {0, 120, 240, 360, 480}},
    // X4 Pro: no front keys; the cells are tapped.
    {false, {Key::Back, Key::Confirm, Key::Left, Key::Right}, {0, 120, 240, 360, 480}},
};

Key keyFromButton(const platform::Button button) {
  switch (button) {
    case platform::Button::Back:
      return Key::Back;
    case platform::Button::Confirm:
      return Key::Confirm;
    case platform::Button::Left:
      return Key::Left;
    case platform::Button::Right:
      return Key::Right;
    case platform::Button::Up:
      return Key::Up;
    case platform::Button::Down:
      return Key::Down;
    case platform::Button::Power:
      return Key::Power;
  }
  return Key::Back;
}

}  // namespace

const char* keyName(const Key key) {
  switch (key) {
    case Key::Back:
      return "back";
    case Key::Confirm:
      return "confirm";
    case Key::Left:
      return "left";
    case Key::Right:
      return "right";
    case Key::Up:
      return "up";
    case Key::Down:
      return "down";
    case Key::Power:
      return "power";
    case Key::Home:
      return "home";
  }
  return "?";
}

void KeyMap::begin(platform::Board& board, const freeink::ui::DeviceContext& device) {
  board_ = &board;
  device_ = device;
  const FrontKeys& row = kFrontKeys[static_cast<uint8_t>(board.device())];
  if (row.edges[kFooterCellCount] != device.width) {
    platform::log("keymap: footer edges end at %d but the screen is %d wide", row.edges[kFooterCellCount],
                  device.width);
  }
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    cells_[i].x = row.edges[i];
    cells_[i].width = static_cast<int16_t>(row.edges[i + 1] - row.edges[i]);
    cells_[i].hasKey = row.present && board.hasFrontKeys();
    cells_[i].key = row.order[i];
  }
}

bool KeyMap::next(InputEvent& out) {
  if (!board_) return false;
  platform::RawInput raw;
  if (!board_->nextInput(raw)) return false;
  out = InputEvent{};
  const bool fx = board_->touchFlipX();
  const bool fy = board_->touchFlipY();
  switch (raw.kind) {
    case platform::RawInput::Kind::Button:
      out.key = keyFromButton(raw.button);
      break;
    case platform::RawInput::Kind::BackHold:
      out.key = Key::Back;
      out.hold = true;
      break;
    case platform::RawInput::Kind::Tap: {
      const Point p = freeink::ui::touchToLogical(device_, raw.nx, raw.ny, fx, fy);
      out.kind = InputEvent::Kind::Tap;
      out.x = p.x;
      out.y = p.y;
      break;
    }
    case platform::RawInput::Kind::Swipe: {
      const Point a = freeink::ui::touchToLogical(device_, raw.nx, raw.ny, fx, fy);
      const Point b = freeink::ui::touchToLogical(device_, raw.nx2, raw.ny2, fx, fy);
      out.kind = InputEvent::Kind::Swipe;
      out.x = a.x;
      out.y = a.y;
      out.x2 = b.x;
      out.y2 = b.y;
      out.swipe = freeink::ui::swipeDirection(a.x, a.y, b.x, b.y);
      out.fromTopEdge =
          freeink::ui::edgeSwipe(freeink::ui::ScreenEdge::Top, a.x, a.y, b.x, b.y, device_.width, device_.height);
      break;
    }
    case platform::RawInput::Kind::HomeTap:
      out.key = Key::Home;
      break;
    case platform::RawInput::Kind::HomeHold:
      out.key = Key::Home;
      out.hold = true;
      break;
  }
  return true;
}

int8_t KeyMap::footerIndexOf(const Key key) const {
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    if (cells_[i].hasKey && cells_[i].key == key) return static_cast<int8_t>(i);
  }
  return -1;
}

int8_t KeyMap::choiceFor(const InputEvent& event, const ChoiceBar& bar, const int16_t barTop) const {
  int8_t index = -1;
  if (event.kind == InputEvent::Kind::Key && !event.hold) {
    index = footerIndexOf(event.key);
  } else if (event.kind == InputEvent::Kind::Tap && event.y >= barTop) {
    for (uint8_t i = 0; i < kFooterCellCount; ++i) {
      if (event.x >= cells_[i].x && event.x < cells_[i].x + cells_[i].width) index = static_cast<int8_t>(i);
    }
  }
  if (index < 0 || bar.cells[index].empty() || !bar.cells[index].enabled) return -1;
  return index;
}

freeink::ui::InputSnapshot KeyMap::snapshotFor(const InputEvent& event) const {
  freeink::ui::InputSnapshot s;
  switch (event.kind) {
    case InputEvent::Kind::Tap:
      s.touchReleased = true;
      s.touchX = event.x;
      s.touchY = event.y;
      break;
    case InputEvent::Kind::Swipe:
      s.swipeLeft = event.swipe == freeink::ui::SwipeDir::Left;
      s.swipeRight = event.swipe == freeink::ui::SwipeDir::Right;
      break;
    case InputEvent::Kind::Key:
      if (event.hold) break;
      switch (event.key) {
        case Key::Confirm:
          s.confirm = true;
          break;
        case Key::Back:
          s.back = true;
          break;
        case Key::Up:
        case Key::Left:
          s.focusPrev = true;
          break;
        case Key::Down:
        case Key::Right:
          s.focusNext = true;
          break;
        default:
          break;
      }
      break;
  }
  return s;
}

void KeyMap::describeOrder(char* out, const size_t cap) const {
  size_t used = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < kFooterCellCount && used < cap; ++i) {
    const int n =
        snprintf(out + used, cap - used, "%s%s", i ? " " : "", cells_[i].hasKey ? keyName(cells_[i].key) : "tap");
    if (n < 0) break;
    used += static_cast<size_t>(n);
  }
}

}  // namespace tinta::ui

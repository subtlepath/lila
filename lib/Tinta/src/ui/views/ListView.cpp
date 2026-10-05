#include "ui/views/ListView.h"

#include "app/App.h"
#include "icons/Icons.h"
#include "platform/Log.h"
#include "ui/Strings.h"

namespace tinta::ui {
namespace {

using freeink::ui::Rect;

enum : app::ActionId { kActionRow = app::kFirstViewAction };

bool swipe(const InputEvent& e, freeink::ui::SwipeDir dir) {
  return e.kind == InputEvent::Kind::Swipe && !e.fromTopEdge && e.swipe == dir;
}

}  // namespace

int16_t ListView::rowHeight() const { return app_.theme().rowHeight; }

void ListView::enter(const bool returning) {
  traceNext_ = true;
  bar_ = ChoiceBar{};
  bar_.cells[2].icon = &icons::kChevronUp24;
  bar_.cells[3].icon = &icons::kChevronDown24;
  if (returning) {
    logCursor();
    return;
  }
  top_ = 0;
  cursor_ = initialRow();
  const uint16_t count = rowCount();
  while (cursor_ < count && !selectable(cursor_)) ++cursor_;
  if (cursor_ >= count) cursor_ = 0;
  placeCursor_ = true;
  logCursor();
}

void ListView::logCursor() const { platform::log("list %s cursor %u", name(), cursor_); }

void ListView::build(app::UiScreen& screen) {
  buildHeader(screen);
  const bool touch = !app_.keyDevice();
  const int16_t rowH = rowHeight();
  const int16_t available = screen.body().height;
  rows_ = static_cast<uint8_t>(available / rowH > 0 ? (available / rowH < 30 ? available / rowH : 30) : 1);
  const uint16_t count = rowCount();
  if (placeCursor_) {
    top_ = static_cast<uint16_t>(cursor_ - cursor_ % rows_);
    placeCursor_ = false;
  }
  for (uint8_t i = 0; i < rows_ && top_ + i < count; ++i) {
    const uint16_t index = static_cast<uint16_t>(top_ + i);
    const Rect row = screen.takeTop(rowH);
    drawRow(screen, row, index, !touch && index == cursor_ && selectable(index));
    if (touch && selectable(index)) {
      screen.frame().hit(row, kActionRow, index, freeink::ui::InputTouch);
      if (platform::kSimulator && traceNext_) {
        platform::log("target %s/row%u %d %d", name(), index, row.x + row.width / 2, row.y + row.height / 2);
      }
    }
  }
  traceNext_ = false;
}

const ChoiceBar* ListView::choiceBar() const {
  if (app_.keyDevice()) return nullptr;
  ChoiceBar& bar = const_cast<ChoiceBar&>(bar_);
  bar.cells[2].enabled = top_ > 0;
  bar.cells[3].enabled = top_ + rows_ < rowCount();
  // A list that fits one page needs no arrows.
  if (top_ == 0 && rows_ >= rowCount()) return nullptr;
  return &bar_;
}

bool ListView::keyHints(ChoiceBar& out) const {
  View::keyHints(out);
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    CellSpec& cell = out.cells[i];
    switch (app_.keys().footerCell(i).key) {
      case Key::Confirm:
        cell = CellSpec{};
        if (cursor_ < rowCount() && selectable(cursor_)) cell.label = tr(Str::Open);
        break;
      case Key::Left:
        cell = CellSpec{};
        if (top_ > 0) cell.icon = &icons::kChevronUp24;
        break;
      case Key::Right:
        cell = CellSpec{};
        if (top_ + rows_ < rowCount()) cell.icon = &icons::kChevronDown24;
        break;
      default:
        break;
    }
  }
  return true;
}

void ListView::move(const int8_t dir) {
  const uint16_t count = rowCount();
  int32_t r = cursor_;
  do {
    r += dir;
    if (r < 0 || r >= count) return;
  } while (!selectable(static_cast<uint16_t>(r)));
  cursor_ = static_cast<uint16_t>(r);
  bool turned = false;
  if (cursor_ < top_) {
    top_ = static_cast<uint16_t>(cursor_ - cursor_ % rows_);
    turned = true;
  }
  if (cursor_ >= top_ + rows_) {
    top_ = static_cast<uint16_t>(cursor_ - cursor_ % rows_);
    turned = true;
  }
  logCursor();
  if (turned) {
    traceNext_ = true;
    app_.invalidateCard();
  } else {
    app_.invalidate();
  }
}

void ListView::turnPage(const int8_t dir) {
  const int32_t next = static_cast<int32_t>(top_) + dir * rows_;
  if (next < 0 || next >= rowCount()) return;
  top_ = static_cast<uint16_t>(next);
  cursor_ = top_;
  const uint16_t count = rowCount();
  while (cursor_ + 1 < count && cursor_ < top_ + rows_ - 1 && !selectable(cursor_)) ++cursor_;
  traceNext_ = true;
  logCursor();
  app_.clearTapFlash();
  app_.invalidateCard();
}

void ListView::onAction(const app::ActionEvent& event) {
  if (event.action == kActionRow && event.value >= 0) {
    const uint16_t index = static_cast<uint16_t>(event.value);
    if (index < rowCount() && selectable(index)) {
      cursor_ = index;
      app_.clearTapFlash();
      activate(index);
    }
    return;
  }
  if (event.action != app::kActionChoice) return;
  if (event.value == 2) turnPage(-1);
  if (event.value == 3) turnPage(1);
}

bool ListView::onInput(const InputEvent& event) {
  if (swipe(event, freeink::ui::SwipeDir::Up)) {
    turnPage(1);
    return true;
  }
  if (swipe(event, freeink::ui::SwipeDir::Down)) {
    turnPage(-1);
    return true;
  }
  if (!app_.keyDevice() || event.kind != InputEvent::Kind::Key || event.hold) return false;
  switch (event.key) {
    case Key::Up:
      move(-1);
      return true;
    case Key::Down:
      move(1);
      return true;
    case Key::Left:
      turnPage(-1);
      return true;
    case Key::Right:
      turnPage(1);
      return true;
    case Key::Confirm:
      if (cursor_ < rowCount() && selectable(cursor_)) activate(cursor_);
      return true;
    default:
      return false;
  }
}

}  // namespace tinta::ui

#include "ui/views/FormView.h"

#include "app/App.h"
#include "core/lang/Charset.h"
#include "icons/Icons.h"
#include "platform/Log.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"

namespace tinta::ui {
namespace {

using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::State;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

TextStyle textStyle(freeink::ui::FontId font, TextAlign align = TextAlign::Left) {
  TextStyle s;
  s.font = font;
  s.align = align;
  return s;
}

}  // namespace

void FormView::enter(const bool returning) {
  traceNext_ = true;
  if (returning) return;
  // The first focusable row at or after initialRow(), as FreeInkUI will pick.
  focusedRow_ = static_cast<int8_t>(initialRow() - 1);
  moveFocus(1);
}

void FormView::build(app::UiScreen& screen) {
  buildHeader(screen);
  buildRows(screen);
}

void FormView::buildRows(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  const bool touch = !app_.keyDevice();
  freeink::ui::DrawTarget& t = screen.target();
  const uint8_t count = rowCount() < kMaxRows ? rowCount() : kMaxRows;
  // Focus stops before each row: what a key device's flow presses Down for.
  uint8_t ordinal = 0;
  if (platform::kSimulator && traceNext_ && !touch) platform::log("focus %s %d", name(), focusOrdinal());

  for (uint8_t i = 0; i < count; ++i) {
    char value[kValueCap] = {};
    RowSpec spec;
    row(i, spec, value, sizeof value);

    if (spec.kind == RowSpec::Kind::Heading) {
      const int16_t lh = t.lineHeight(kSlotSmall);
      const Rect rect = screen.takeTop(i16(lh + 14));
      const Rect text{i16(rect.x + theme.margin), i16(rect.y + 8), i16(rect.width - 2 * theme.margin), lh};
      t.text(text, spec.label, textStyle(kSlotKeyLabel));
      t.fill(Rect{text.x, i16(text.bottom() + 2), text.width, 1}, Paint::solid(Color::Black));
      continue;
    }

    const Rect rect = screen.takeTop(theme.rowHeight, touch ? 4 : 0);
    if (rect.empty()) break;
    if (!touch && spec.kind != RowSpec::Kind::Info && spec.enabled) traceRow(spec.slug, ordinal++);

    if (touch && spec.kind == RowSpec::Kind::Stepper) {
      drawStepper(screen, rect, i, spec);
    } else if (touch && spec.kind == RowSpec::Kind::Toggle) {
      freeink::ui::ToggleRowProps p;
      p.row.label = spec.label;
      p.row.action = kActionRow;
      p.row.valueId = i;
      p.row.labelText = theme.tokens.bodyText;
      p.row.styles = theme.tokens.listRow;
      p.row.sidePadding = theme.margin;
      p.row.minTouchSize = theme.tokens.minTouchSize;
      p.row.enabled = spec.enabled;
      p.checked = spec.checked;
      p.toggleWidth = 52;
      p.toggleHeight = 28;
      p.knobInset = 4;
      p.borderWidth = 2;
      freeink::ui::toggleRow(screen.frame(), rect, p);
      trace(spec.slug, "", rect);
    } else {
      freeink::ui::SettingRowProps p;
      p.label = spec.label;
      p.value = spec.kind == RowSpec::Kind::Toggle ? tr(spec.checked ? Str::On : Str::Off) : spec.value;
      p.action = spec.kind == RowSpec::Kind::Info ? freeink::ui::NO_ACTION : kActionRow;
      p.valueId = i;
      p.labelText = theme.tokens.bodyText;
      p.valueText = textStyle(kSlotBodyBold);
      p.styles = theme.tokens.listRow;
      p.enabled = spec.enabled;
      p.sidePadding = theme.margin;
      p.minTouchSize = theme.tokens.minTouchSize;
      p.drawChevron = spec.kind == RowSpec::Kind::Link;
      freeink::ui::settingRow(screen.frame(), rect, p);
      if (spec.kind != RowSpec::Kind::Info) trace(spec.slug, "", rect);
    }

    if (!touch && spec.kind != RowSpec::Kind::Info &&
        freeink::ui::hasState(screen.frame().stateFor(kActionRow, i), freeink::ui::StateFocused)) {
      focusedRow_ = static_cast<int8_t>(i);
    }
    // A hairline between rows; the focus fill covers it on key devices.
    t.fill(
        Rect{i16(rect.x + theme.margin), i16(rect.bottom() + (touch ? 1 : 0)), i16(rect.width - 2 * theme.margin), 1},
        Paint::dither(Color::LightGray));
  }
  traceNext_ = false;
}

void FormView::drawStepper(app::UiScreen& screen, const Rect rect, const uint8_t index, const RowSpec& spec) {
  const Theme& theme = app_.theme();
  freeink::ui::DrawTarget& t = screen.target();
  constexpr int16_t kButtonW = 56;
  constexpr int16_t kValueW = 112;
  const int16_t buttonH = i16(rect.height - 8);
  const int16_t y = i16(rect.y + (rect.height - buttonH) / 2);
  const Rect plus{i16(rect.right() - theme.margin - kButtonW), y, kButtonW, buttonH};
  const Rect valueRect{i16(plus.x - kValueW), rect.y, kValueW, rect.height};
  const Rect minus{i16(valueRect.x - kButtonW), y, kButtonW, buttonH};

  t.text(Rect{i16(rect.x + theme.margin), rect.y, i16(minus.x - rect.x - theme.margin - theme.gap), rect.height},
         spec.label, theme.tokens.bodyText);
  t.text(valueRect, spec.value, textStyle(kSlotBodyBold, TextAlign::Center));

  freeink::ui::ButtonProps b;
  b.styles = theme.tokens.button;
  b.text = textStyle(kSlotTitle, TextAlign::Center);
  b.minTouchSize = theme.tokens.minTouchSize;
  b.enabled = spec.enabled;
  b.radius = 8;
  b.label = TINTA_EN_DASH;
  b.action = kActionDecrement;
  b.value = index;
  freeink::ui::button(screen.frame(), minus, b);
  b.label = "+";
  b.action = kActionIncrement;
  freeink::ui::button(screen.frame(), plus, b);
  trace(spec.slug, "-", minus);
  trace(spec.slug, "+", plus);
}

void FormView::trace(const char* slug, const char* suffix, const Rect rect) const {
  if (!platform::kSimulator || !traceNext_ || app_.keyDevice() || !slug) return;
  platform::log("target %s/%s%s %d %d", name(), slug, suffix, rect.x + rect.width / 2, rect.y + rect.height / 2);
}

void FormView::traceRow(const char* slug, const uint8_t ordinal) const {
  if (!platform::kSimulator || !traceNext_ || !slug) return;
  platform::log("row %s/%s %u", name(), slug, ordinal);
}

bool FormView::adjustable(const uint8_t index) const {
  char value[kValueCap];
  RowSpec spec;
  row(index, spec, value, sizeof value);
  return spec.enabled && (spec.kind == RowSpec::Kind::Stepper || spec.kind == RowSpec::Kind::Choice ||
                          spec.kind == RowSpec::Kind::Toggle);
}

void FormView::onAction(const app::ActionEvent& event) {
  if (event.value < 0 || event.value >= rowCount()) return;
  const auto index = static_cast<uint8_t>(event.value);
  switch (event.action) {
    case kActionDecrement:
      step(index, -1);
      break;
    case kActionIncrement:
      step(index, 1);
      break;
    case kActionRow: {
      char value[kValueCap];
      RowSpec spec;
      row(index, spec, value, sizeof value);
      if (spec.kind == RowSpec::Kind::Link || spec.kind == RowSpec::Kind::Action) {
        focusedRow_ = static_cast<int8_t>(index);
        activate(index);
      } else {
        step(index, 1);
      }
      break;
    }
    default:
      return;
  }
  app_.invalidate();
}

bool FormView::onInput(const InputEvent& event) {
  if (!app_.keyDevice() || event.kind != InputEvent::Kind::Key || event.hold) return false;
  const bool onRow = focusedRow_ >= 0 && focusedRow_ < rowCount();
  if ((event.key == Key::Left || event.key == Key::Right) && onRow && adjustable(static_cast<uint8_t>(focusedRow_))) {
    step(static_cast<uint8_t>(focusedRow_), event.key == Key::Left ? -1 : 1);
    app_.invalidate();
    return true;
  }
  // A focus move. FreeInkUI moves its focus when the app routes the key; follow
  // it here too, because several keys can arrive before the next build. Rows
  // are the only focusable elements, in row order, so the two agree.
  int8_t dir = 0;
  if (event.key == Key::Up || event.key == Key::Left) dir = -1;
  if (event.key == Key::Down || event.key == Key::Right) dir = 1;
  if (dir != 0) moveFocus(dir);
  return false;
}

void FormView::moveFocus(const int8_t dir) {
  const int count = rowCount();
  if (count == 0) return;
  int at = focusedRow_ < 0 ? (dir > 0 ? -1 : count) : focusedRow_;
  for (int stepCount = 0; stepCount < count; ++stepCount) {
    at = (at + dir + count) % count;
    char value[kValueCap];
    RowSpec spec;
    row(static_cast<uint8_t>(at), spec, value, sizeof value);
    if (spec.enabled && spec.kind != RowSpec::Kind::Info && spec.kind != RowSpec::Kind::Heading) {
      focusedRow_ = static_cast<int8_t>(at);
      return;
    }
  }
}

bool FormView::keyHints(ChoiceBar& out) const {
  if (!View::keyHints(out)) return false;
  if (focusedRow_ < 0 || focusedRow_ >= rowCount()) return true;
  char value[kValueCap];
  RowSpec spec;
  row(static_cast<uint8_t>(focusedRow_), spec, value, sizeof value);
  const bool canStep = adjustable(static_cast<uint8_t>(focusedRow_));
  for (uint8_t i = 0; i < kFooterCellCount; ++i) {
    CellSpec& cell = out.cells[i];
    switch (app_.keys().footerCell(i).key) {
      case Key::Confirm:
        cell.label = spec.kind == RowSpec::Kind::Link     ? tr(Str::Open)
                     : spec.kind == RowSpec::Kind::Action ? tr(Str::Select)
                                                          : tr(Str::Change);
        break;
      case Key::Left:
        cell.icon = canStep ? &icons::kChevronLeft24 : &icons::kChevronUp24;
        break;
      case Key::Right:
        cell.icon = canStep ? &icons::kChevronRight24 : &icons::kChevronDown24;
        break;
      default:
        break;
    }
  }
  return true;
}

int8_t FormView::focusOrdinal() const {
  if (focusedRow_ < 0) return 0;
  int8_t ordinal = 0;
  for (uint8_t i = 0; i < focusedRow_ && i < rowCount(); ++i) {
    char value[kValueCap];
    RowSpec spec;
    row(i, spec, value, sizeof value);
    if (spec.enabled && spec.kind != RowSpec::Kind::Info && spec.kind != RowSpec::Kind::Heading) ++ordinal;
  }
  return ordinal;
}

}  // namespace tinta::ui

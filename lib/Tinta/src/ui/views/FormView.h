#pragma once

// A screen of rows: menus (Home, Settings, Diagnostics), settings pages, the
// date and clock pickers and the sheets all derive from it. A subclass
// describes rows; FormView draws them for the device and routes input:
//
//   Key devices  one focus stop per row; Up/Down (side keys) move focus,
//                Left/Right step the focused value, Confirm opens, toggles,
//                cycles or runs it. The footer says which.
//   X4 Pro       steppers get - and + buttons; toggles a switch; every other
//                row is tapped.
//
// In simulator builds, touch devices log "[tinta] target <screen>/<row> x y"
// for each row (and "<row>-", "<row>+" for stepper buttons) when a screen
// appears, so flows can tap by name; key devices log "[tinta] row
// <screen>/<row> <n>", n being the focus steps from the first row, and
// "[tinta] focus <screen> <n>" for the row focused when it appears.

#include <stdint.h>

#include "app/View.h"

namespace tinta::ui {

struct RowSpec {
  enum class Kind : uint8_t {
    Link,     // opens something; drawn with a chevron
    Action,   // runs something
    Stepper,  // a value stepped down and up
    Choice,   // one of a few values; Confirm or a tap moves to the next
    Toggle,   // on or off
    Info,     // a label and a value, not interactive
    Heading,  // a section title
  };
  Kind kind = Kind::Link;
  const char* label = nullptr;
  const char* value = nullptr;  // formatted by the subclass
  const char* slug = nullptr;   // ASCII id for logs and flows
  bool checked = false;         // Toggle
  bool enabled = true;
};

class FormView : public app::View {
 public:
  using View::View;

  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override;

 protected:
  static constexpr uint8_t kMaxRows = 12;
  static constexpr size_t kValueCap = 64;

  virtual uint8_t rowCount() const = 0;
  // Fills `out`; `value` is scratch space for the formatted value.
  virtual void row(uint8_t index, RowSpec& out, char* value, size_t cap) const = 0;
  // Stepper: dir -1 or +1. Choice: +1 moves on (wrapping), -1 back. Toggle: flips.
  virtual void step(uint8_t index, int8_t dir) { (void)index, (void)dir; }
  // Link and Action rows.
  virtual void activate(uint8_t index) { (void)index; }
  // The row focused when the screen is first shown.
  virtual uint8_t initialRow() const { return 0; }

  // Content above the rows (text, banners); default none.
  virtual void buildHeader(app::UiScreen& screen) { (void)screen; }
  void buildRows(app::UiScreen& screen);

  int8_t focusedRow() const { return focusedRow_; }

  enum : app::ActionId {
    kActionRow = app::kFirstViewAction,
    kActionDecrement,
    kActionIncrement,
  };

 private:
  bool adjustable(uint8_t index) const;
  void moveFocus(int8_t dir);
  void drawStepper(app::UiScreen& screen, freeink::ui::Rect rect, uint8_t index, const RowSpec& spec);
  void trace(const char* slug, const char* suffix, freeink::ui::Rect rect) const;
  void traceRow(const char* slug, uint8_t ordinal) const;

  int8_t focusedRow_ = -1;
  bool traceNext_ = false;
};

}  // namespace tinta::ui

#pragma once

// The date and the clock (PLAN.md 6.8).
//
// DatePromptScreen  the X4 at every power-on: "What day is it?" with a choice
//                   bar, Same day / Next day / Other date: one press in the
//                   usual cases.
// DatePickerScreen  day, month and year: the X4's first run, "Other date", and
//                   Settings on the X4.
// SetClockScreen    date and time on RTC devices: first run, a clock that
//                   reads nonsense, and Settings.
//
// At boot these are the bottom of the stack and finish with
// App::finishTimeStep(); opened from Settings they pop.

#include "app/View.h"
#include "core/Date.h"
#include "platform/Clock.h"
#include "ui/views/FormView.h"

namespace tinta::ui {

class DatePromptScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "date-prompt"; }
  const char* title() const override;
  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  void onBack() override {}
  const ChoiceBar* choiceBar() const override { return &bar_; }
  bool allowsGlobalGestures() const override { return false; }
  bool restorable() const override { return false; }

 private:
  core::DayNumber last_ = 0;
  ChoiceBar bar_{};
  char sameText_[24] = {};
  char nextText_[24] = {};
};

class DatePickerScreen final : public FormView {
 public:
  using FormView::FormView;

  const char* name() const override { return "date-picker"; }
  const char* title() const override;
  void enter(bool returning) override;
  bool allowsGlobalGestures() const override;
  bool restorable() const override { return false; }

 protected:
  uint8_t rowCount() const override;
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void step(uint8_t index, int8_t dir) override;
  void activate(uint8_t index) override;
  void buildHeader(app::UiScreen& screen) override;

 private:
  core::date::Civil date_{};
};

class SetClockScreen final : public FormView {
 public:
  using FormView::FormView;

  const char* name() const override { return "set-clock"; }
  const char* title() const override;
  void enter(bool returning) override;
  bool allowsGlobalGestures() const override;
  bool restorable() const override { return false; }

 protected:
  uint8_t rowCount() const override;
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void step(uint8_t index, int8_t dir) override;
  void activate(uint8_t index) override;
  void buildHeader(app::UiScreen& screen) override;

 private:
  platform::LocalTime time_{};
  bool hadReading_ = false;
  platform::LocalTime reading_{};
};

}  // namespace tinta::ui

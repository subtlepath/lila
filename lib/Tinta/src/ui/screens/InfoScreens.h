#pragma once

// About and its licences, Diagnostics and the type specimen (PLAN.md 5.6,
// 11).

#include "app/View.h"
#include "ui/views/FormView.h"

namespace tinta::ui {

// The name, version, device and course, then How the keys work (the first
// run's guide again) and Licences.
class AboutScreen final : public FormView {
 public:
  using FormView::FormView;

  const char* name() const override { return "about"; }
  const char* title() const override;

 protected:
  uint8_t rowCount() const override { return 2; }
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void activate(uint8_t index) override;
  void buildHeader(app::UiScreen& screen) override;
};

// The third-party notices (NOTICE), one to a page: the Adobe bitmap fonts,
// the larger type, the software, the MIT License, the course's sources.
// Legal text stays in English; the headings and the course page follow the
// interface language. Left and Right (or the side keys) turn pages; on the
// X4 Pro the footer's arrows or a swipe.
class LicencesScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "licences"; }
  const char* title() const override;
  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  const ChoiceBar* choiceBar() const override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override { return -1; }

 private:
  void turn(int8_t dir);

  uint8_t page_ = 0;
  ChoiceBar bar_{};
};

// Links to the input test and the specimen, then what the device knows about
// itself: card, clock, today, the profile's course position, board.
class DiagnosticsScreen final : public FormView {
 public:
  using FormView::FormView;

  const char* name() const override { return "diagnostics"; }
  const char* title() const override;

 protected:
  uint8_t rowCount() const override;
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void activate(uint8_t index) override;

 private:
  // The full CRC check of the course pack: 0 not run, 1 intact, 2 damaged.
  uint8_t packCheck_ = 0;
};

// In place of Home when the course pack built into the firmware is missing
// or fails its structural checks: what happened, and Settings and Sleep.
class PackErrorScreen final : public FormView {
 public:
  using FormView::FormView;

  const char* name() const override { return "pack-error"; }
  const char* title() const override;
  // Leaves Tinta: there is nothing to go back to.
  void onBack() override;

 protected:
  uint8_t rowCount() const override { return 2; }
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void activate(uint8_t index) override;
  void buildHeader(app::UiScreen& screen) override;
};

}  // namespace tinta::ui

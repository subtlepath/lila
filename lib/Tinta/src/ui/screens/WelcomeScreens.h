#pragma once

// The first run (PLAN.md 4.5, M7): a new card starts here instead of Home.
//
// WelcomeScreen       the interface language, asked in both languages; then
//                     the date or the clock where the device needs one
//                     (App::beginTimeStep), then
// KeyGuideScreen      how the keys work on this device, or the touch screen
//                     on the X4 Pro; Next goes on to
// VulgarChoiceScreen  vulgar words, off unless turned on here; Start goes
//                     Home.
//
// Back steps back through them. They are restorable: a sleep halfway comes
// back to the same page. The key guide is also Settings > About > How the
// keys work, where it only goes back.

#include "app/View.h"
#include "ui/views/FormView.h"

namespace tinta::ui {

class WelcomeScreen final : public FormView {
 public:
  using FormView::FormView;

  const char* name() const override { return "welcome"; }
  const char* title() const override;
  void onBack() override {}
  bool allowsGlobalGestures() const override { return false; }

 protected:
  uint8_t rowCount() const override { return 3; }
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void activate(uint8_t index) override;
  uint8_t initialRow() const override;
  void buildHeader(app::UiScreen& screen) override;
};

class KeyGuideScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "key-guide"; }
  const char* title() const override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  const ChoiceBar* choiceBar() const override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override { return -1; }
  bool allowsGlobalGestures() const override;

 private:
  bool firstRun() const;
  void next();

  ChoiceBar bar_{};
};

class VulgarChoiceScreen final : public FormView {
 public:
  using FormView::FormView;

  const char* name() const override { return "vulgar-choice"; }
  const char* title() const override;
  bool allowsGlobalGestures() const override { return false; }

 protected:
  uint8_t rowCount() const override { return 2; }
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void step(uint8_t index, int8_t dir) override;
  void activate(uint8_t index) override;
  // Start: Confirm goes on with vulgar words off.
  uint8_t initialRow() const override { return 1; }
  void buildHeader(app::UiScreen& screen) override;
};

}  // namespace tinta::ui

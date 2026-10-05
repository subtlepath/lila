#pragma once

// Sheets drawn over the current screen.
//
// PauseSheet  Resume, End session (over a session), Home, Settings, Light
//             (X4 Pro), Sleep. Opened by holding Back on key devices and by
//             the Home pad on the X4 Pro; the same gesture, Back or a tap
//             outside closes it.
// LightSheet  X4 Pro frontlight brightness and warmth. Opened by a swipe down
//             from the top edge, from the pause sheet or from Settings.

#include "app/View.h"
#include "ui/views/FormView.h"

namespace tinta::ui {

class PauseSheet final : public FormView {
 public:
  using FormView::FormView;

  const char* name() const override { return "pause"; }
  Kind kind() const override { return Kind::Overlay; }
  bool restorable() const override { return false; }
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;

 protected:
  uint8_t rowCount() const override;
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void activate(uint8_t index) override;
};

}  // namespace tinta::ui

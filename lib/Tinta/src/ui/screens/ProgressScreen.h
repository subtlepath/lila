#pragma once

// Progress (PLAN.md 4.5, M4): the streak; items not started, learning,
// mature and suspended; reviews over the last 14 days and the due forecast
// for the next 14 as bars; time studied. Read once when the screen opens
// (one pass over items.bin and one over days.bin).

#include "app/View.h"
#include "core/Clock.h"

namespace tinta::ui {

class ProgressScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "progress"; }
  const char* title() const override;
  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override { return -1; }

 private:
  static constexpr uint8_t kDays = 14;

  uint16_t streak_ = 0;
  uint32_t notStarted_ = 0;
  uint32_t learning_ = 0;
  uint32_t mature_ = 0;
  uint32_t suspended_ = 0;
  uint16_t past_[kDays] = {};   // reviews, oldest first, today last
  uint16_t ahead_[kDays] = {};  // due, today (with overdue) first
  uint32_t seconds_ = 0;
  uint32_t studyDays_ = 0;
};

}  // namespace tinta::ui

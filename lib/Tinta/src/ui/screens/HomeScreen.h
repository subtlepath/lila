#pragma once

// Home: today's date, the storage banner when there is no card, the Today
// card (due and new counts, the streak) and the menu: Start or Continue, the
// current lesson, Course map, Read, Phrases, Dictionary, Progress, Settings,
// Sleep. The menu is the table kEntries in HomeScreen.cpp; an entry is one
// line there.

#include "core/Clock.h"
#include "ui/views/FormView.h"

namespace tinta::ui {

class HomeScreen final : public FormView {
 public:
  using FormView::FormView;

  const char* name() const override { return "home"; }
  const char* title() const override;
  void onBack() override {}

 protected:
  uint8_t rowCount() const override;
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void activate(uint8_t index) override;
  void buildHeader(app::UiScreen& screen) override;

 private:
  // The streak scans days.bin; it is kept until the day or the journal moves.
  bool streakValid_ = false;
  uint16_t streak_ = 0;
  core::DayNumber streakDay_ = 0;
  uint32_t streakJournal_ = 0;
};

// The banner Home and Settings show when progress cannot be saved, or null.
const char* storageBanner(app::App& app);

}  // namespace tinta::ui

#pragma once

// The phrasebook (PLAN.md 4.5, M6).
//
// PhrasebookScreen  the categories, each with its English name and how many
//                   phrases it has.
// PhrasesScreen     one category's phrases as cards, as many whole cards as
//                   fit a page: the Spanish, its respelling, the English, and
//                   the register and usage note where there is one. Practise
//                   runs the category's phrase items as a practice session
//                   (DayQueue::buildPractice), graded and journalled like any
//                   review. Vulgar phrases are left out unless "show vulgar
//                   words" is on.
//
// Simulator logs: "[tinta] phrases <category> page <p>/<n>".

#include "app/View.h"
#include "ui/views/CardText.h"
#include "ui/views/ListView.h"

namespace tinta::ui {

class PhrasebookScreen final : public ListView {
 public:
  using ListView::ListView;

  const char* name() const override { return "phrasebook"; }
  const char* title() const override;

 protected:
  uint16_t rowCount() const override;
  void drawRow(app::UiScreen& screen, freeink::ui::Rect row, uint16_t index, bool focused) override;
  void activate(uint16_t index) override;
  int16_t rowHeight() const override;
};

class PhrasesScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "phrases"; }
  const char* title() const override;
  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  const ChoiceBar* choiceBar() const override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override { return -1; }
  bool restorable() const override { return false; }

 private:
  static constexpr uint8_t kMaxEntries = 48;
  static constexpr uint8_t kMaxPages = 24;

  bool drawCard(freeink::ui::DisplayTarget& t, Column& col, uint8_t entry);
  void turn(int8_t dir);
  void practise();
  void logPage() const;

  uint16_t category_ = 0;
  uint16_t entries_[kMaxEntries] = {};  // PENT indices, vulgar ones left out
  uint8_t entryCount_ = 0;
  uint8_t pageStart_[kMaxPages + 1] = {};
  uint8_t pageCount_ = 1;
  uint8_t page_ = 0;
  int16_t laidOutHeight_ = -1;
  ChoiceBar bar_{};
  mutable char title_[48] = {};
};

}  // namespace tinta::ui

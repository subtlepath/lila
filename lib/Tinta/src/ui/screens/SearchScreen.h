#pragma once

// Dictionary search on the X4 Pro (PLAN.md 8.5, M6): the query, the results
// under it, and the Spanish keyboard (accent row included) at the bottom.
// Each key runs core::search::Search again: headwords spelled so, inflected
// forms ("fui": ir, ser), headwords that start so, then English senses;
// accents and case do not matter. Tap a result for its entry; OK opens the
// first. A vulgar word with "show vulgar words" off is listed with "vulgar"
// in place of its gloss, as in the dictionary.
//
// Taps on results are resolved against the rows drawn (the keyboard alone
// takes most of a frame's touch targets). Simulator logs: "[tinta] search
// '<query>' <count>", "[tinta] target search/row<i> x y" and the keyboard's
// "[tinta] target key/<label> x y".

#include "app/View.h"
#include "core/search/Search.h"
#include "ui/views/SpanishKeyboard.h"

namespace tinta::ui {

class SearchScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "search"; }
  const char* title() const override;
  void enter(bool returning) override;
  void leave() override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  bool keyHints(ChoiceBar& out) const override { return false; }
  int8_t focusOrdinal() const override { return -1; }
  bool restorable() const override { return false; }

 private:
  static constexpr uint8_t kMaxRows = 12;
  void run();
  void open(uint8_t row);

  char query_[48] = {};
  // The query changed since the usage log last had it.
  bool unrecorded_ = false;
  core::search::Search search_;
  SpanishKeyboard keyboard_{app::kFirstViewAction};
  freeink::ui::Rect rows_[kMaxRows] = {};
  uint8_t rowCount_ = 0;
  bool traceNext_ = false;
};

}  // namespace tinta::ui

#pragma once

// The review session's screens (PLAN.md 4.5, 4.6).
//
// SessionScreen  the status line ("Review 12/40", "Lesson 1.2 3/15"), a
//                progress bar, the card (drawn by the item's
//                ui::ExerciseView) and the footer.
//                Self-graded cards:
//                front   the four grades, greyed out; any front key, side
//                        Down or a tap on the card reveals.
//                back    the four grades, the next interval over each; side
//                        Down is Good.
//                Auto-graded cards: the view's answers over the footer (or
//                its own keys and touch targets); the result keeps that
//                footer, and any key or tap goes on.
//                A reveal or a result only adds ink (a fast refresh leaves no
//                ghost); the next card is a new screen's half refresh.
//                Side Up (or a swipe right) takes back the last grade. Hold
//                Back, or the status bar arrow, for the pause sheet, which
//                has End session.
//                A leech shows a notice with Keep and Suspend first.
// SummaryScreen  reviewed, correct, new, time and the streak; after a
//                lesson's practice, that the lesson is complete and which is
//                next (a row opens it). Done goes Home.

#include "app/View.h"
#include "core/srs/Fsrs.h"
#include "ui/views/ExerciseView.h"
#include "ui/views/FormView.h"

namespace tinta::ui {

class SessionScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "session"; }
  const char* title() const override;
  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  void onBack() override;
  const ChoiceBar* choiceBar() const override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override { return -1; }

 private:
  bool selfGradedFront() const;
  bool autoFront() const;
  bool showingResult() const;
  void reveal();
  void grade(uint8_t cell);
  void answered(core::Grade grade);
  void goOn();
  void reply(ExerciseView::Reply r, core::Grade grade);
  void undo();
  void resolveLeech(bool suspend);
  // After any change: the next card, the same card redrawn, or the summary.
  void advance(bool newCard);
  void fillGrades();
  void buildLeech(app::UiScreen& screen, freeink::ui::Rect area);

  ChoiceBar bar_{};
  ChoiceBar frontBar_{};
  char details_[4][16] = {};
  mutable char title_[32] = {};
  bool traceNext_ = false;
};

class SummaryScreen final : public FormView {
 public:
  using FormView::FormView;

  const char* name() const override { return "summary"; }
  const char* title() const override;
  void enter(bool returning) override;
  void onBack() override;
  bool restorable() const override { return false; }

 protected:
  uint8_t rowCount() const override;
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void drawLessonDone(app::UiScreen& screen);
  void activate(uint8_t index) override;
  void buildHeader(app::UiScreen& screen) override;

 private:
  uint16_t streak_ = 0;
};

}  // namespace tinta::ui

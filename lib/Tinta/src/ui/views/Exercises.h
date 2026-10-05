#pragma once

// The auto-graded exercise views (PLAN.md 4.3). Each answers with one press
// on key devices and one tap on the X4 Pro, except word order (a press per
// tile) and typing (touch only).
//
// ChoiceView     ChooseMeaning, ChooseWord, ChooseGap, ChooseForm and
//                ChooseArticle: the prompt, then two to four numbered options;
//                the front key (or footer cell) under number n answers n, or
//                tap the option. The result marks the right option with a
//                check and a wrong choice with a cross, and adds the answer
//                in full and its note below.
// WordOrderView  BuildSentence: the English, the sentence so far, and the
//                words as tiles. Left/Right move the cursor, Confirm places
//                the word; on the X4 Pro tap a tile. A word out of turn counts
//                as a mistake and stays on the tray.
// TypedView      TypeWord, TypeGap, TypeForm (the X4 Pro, with typed answers
//                on): the prompt, the answer as typed and a Spanish keyboard
//                with a row of accented letters. OK checks it (core/lang
//                AnswerCheck): accents or one slip on a long word are right
//                but Hard, a non-Mexican synonym is right and the Mexican word
//                shown.

#include "core/session/Exercise.h"
#include "ui/views/CardText.h"
#include "ui/views/ExerciseView.h"
#include "ui/views/SpanishKeyboard.h"

namespace tinta::ui {

class ChoiceView final : public ExerciseView {
 public:
  bool accepts(const CardInput& card) const override;
  bool load(const CardInput& card) override;
  ExerciseFormat journalFormat() const override;
  void draw(app::UiScreen& screen, freeink::ui::Rect area, bool back) override;
  bool autoGraded() const override { return true; }
  const ChoiceBar* answers() const override { return &bar_; }
  uint8_t optionTexts(const char** out, uint8_t cap, uint8_t& answer) const override;
  AnswerDetail answerDetail() const override;
  Reply onChoice(uint8_t cell, core::Grade& grade) override;
  Reply onAction(const app::ActionEvent& event, core::Grade& grade) override;

 private:
  void drawPrompt(freeink::ui::DisplayTarget& t, Column& col);
  void drawExplanation(freeink::ui::DisplayTarget& t, Column& col);
  Reply choose(uint8_t option, core::Grade& grade);

  CardInput card_{};
  core::session::OptionSet options_{};
  core::pack::Lemma lemma_{};
  core::pack::Sentence sentence_{};
  bool hasLemma_ = false;
  bool hasSentence_ = false;
  int8_t chosen_ = -1;
  ChoiceBar bar_{};
  char label_[96] = {};
  char texts_[core::session::OptionSet::kMax][64] = {};
  char cells_[core::session::OptionSet::kMax][4] = {};
};

class WordOrderView final : public ExerciseView {
 public:
  bool accepts(const CardInput& card) const override;
  bool load(const CardInput& card) override;
  ExerciseFormat journalFormat() const override { return ExerciseFormat::BuildSentence; }
  void draw(app::UiScreen& screen, freeink::ui::Rect area, bool back) override;
  bool autoGraded() const override { return true; }
  // The tray empties as words are placed: the result cleans up with a new
  // screen's refresh.
  bool resultIsNewScreen() const override { return true; }
  bool keyHints(const KeyMap& keys, ChoiceBar& out) const override;
  AnswerDetail answerDetail() const override;
  Reply onKey(const InputEvent& event, core::Grade& grade) override;
  Reply onAction(const app::ActionEvent& event, core::Grade& grade) override;

 private:
  Reply pick(uint8_t tile, core::Grade& grade);
  void drawBuilt(freeink::ui::DisplayTarget& t, Column& band) const;
  void drawTray(app::UiScreen& screen, freeink::ui::DisplayTarget& t, Column& col);
  void moveCursor(int8_t dir);
  void logCursor() const;

  CardInput card_{};
  core::pack::Sentence sentence_{};
  core::session::WordOrder order_;
  uint8_t cursor_ = 0;
  int8_t lastWrong_ = -1;
  char label_[64] = {};
};

class TypedView final : public ExerciseView {
 public:
  bool accepts(const CardInput& card) const override;
  bool load(const CardInput& card) override;
  ExerciseFormat journalFormat() const override;
  void draw(app::UiScreen& screen, freeink::ui::Rect area, bool back) override;
  bool autoGraded() const override { return true; }
  bool resultIsNewScreen() const override { return true; }
  AnswerDetail answerDetail() const override;
  Reply onAction(const app::ActionEvent& event, core::Grade& grade) override;

 private:
  void drawPrompt(freeink::ui::DisplayTarget& t, Column& col);

  CardInput card_{};
  core::pack::Lemma lemma_{};
  core::pack::Sentence sentence_{};
  bool hasLemma_ = false;
  bool hasSentence_ = false;
  core::lang::Verdict verdict_ = core::lang::Verdict::Wrong;
  bool answered_ = false;
  // Its keys: kExerciseAction + 1 .. + 3.
  SpanishKeyboard keyboard_{static_cast<app::ActionId>(kExerciseAction + 1)};
  char label_[96] = {};
  char expected_[64] = {};
  char typed_[64] = {};
};

}  // namespace tinta::ui

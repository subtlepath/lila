#pragma once

// The self-graded exercise views (PLAN.md 4.3, 4.5), for the Flashcard
// format: mature items, phrases, and any item a choice format cannot ask.
//
// FlashcardView   vocabulary items: Spanish -> English (recognise) and
//                 English -> Spanish (produce). The card of the PLAN 4.5
//                 mock-up: label line, headword, respelling, gloss, example
//                 with the word in bold, its translation, the usage note.
// RevealCardView  every other kind: a phrase asks for the Spanish of its
//                 English; a cloze shows the sentence with a gap and its
//                 English, then the whole sentence; gender asks el or la; a
//                 conjugation asks for one form; word order shows the words
//                 shuffled.

#include "ui/views/CardText.h"
#include "ui/views/ExerciseView.h"

namespace tinta::ui {

class FlashcardView final : public ExerciseView {
 public:
  ExerciseFormat journalFormat() const override;
  bool accepts(const CardInput& card) const override;
  bool load(const CardInput& card) override;
  void draw(app::UiScreen& screen, freeink::ui::Rect area, bool back) override;

 private:
  CardInput card_{};
  core::pack::Lemma lemma_{};
  core::pack::Sentence example_{};
  bool hasExample_ = false;
  char label_[96] = {};
  char headword_[64] = {};
};

class RevealCardView final : public ExerciseView {
 public:
  ExerciseFormat journalFormat() const override { return ExerciseFormat::RevealCard; }
  bool accepts(const CardInput& card) const override;
  bool load(const CardInput& card) override;
  void draw(app::UiScreen& screen, freeink::ui::Rect area, bool back) override;

 private:
  void drawCloze(freeink::ui::DisplayTarget& t, Column& col, bool back);
  void drawGender(freeink::ui::DisplayTarget& t, Column& col, bool back);
  void drawConjugation(freeink::ui::DisplayTarget& t, Column& col, bool back);
  void drawWordOrder(freeink::ui::DisplayTarget& t, Column& col, bool back);
  void drawPhrase(freeink::ui::DisplayTarget& t, Column& col, bool back);

  CardInput card_{};
  core::pack::Lemma lemma_{};
  core::pack::Sentence sentence_{};
  bool hasLemma_ = false;
  bool hasSentence_ = false;
  char label_[96] = {};
  char text_[200] = {};  // headword, the answer form, or the shuffled words
};

}  // namespace tinta::ui

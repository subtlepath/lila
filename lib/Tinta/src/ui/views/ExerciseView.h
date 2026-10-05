#pragma once

// The pluggable part of a review (PLAN.md 4.3, 4.5): how one item is asked
// and answered. app::SessionController owns the session (queue, grades,
// journal, undo, resume) and picks the format (core::session::pickFormat);
// the session screen (ui/screens/SessionScreen) draws the chrome and routes
// input. An ExerciseView draws one card and, for auto-graded formats, turns
// the learner's answer into a grade. docs/app-shell.md describes how a
// format is added.
//
// A card has two sides. The front asks; the back shows the answer.
//   Self-graded views (flashcards, the reveal card): any front key or a tap
//   reveals the back, and the footer offers the four grades.
//   Auto-graded views (choices, word order, typing): an answer on the front
//   is graded by the view, journalled at once, and the back is the result:
//   the same card with right and wrong marked, the correct answer and the
//   note. The next press goes on.
// Either way an item costs two presses and two refreshes (word order takes
// one press per tile).

#include <FreeInkUIDisplayTarget.h>
#include <stdint.h>

#include "app/View.h"
#include "core/pack/Pack.h"
#include "core/session/Exercise.h"
#include "core/srs/Fsrs.h"
#include "ui/Fonts.h"
#include "ui/KeyMap.h"

namespace tinta::ui {

// Journalled with each grade (JournalEntry format, 5 bits). Stored values:
// append only.
enum class ExerciseFormat : uint8_t {
  Unknown = 0,
  FlashcardRecognise = 1,  // Spanish -> English, self-graded
  FlashcardProduce = 2,    // English -> Spanish, self-graded
  RevealCard = 3,          // any other kind, self-graded
  ChooseMeaning = 4,
  ChooseWord = 5,
  ChooseGap = 6,
  ChooseArticle = 7,
  ChooseForm = 8,
  BuildSentence = 9,
  TypeWord = 10,
  TypeGap = 11,
  TypeForm = 12,
};

// The item a card shows, and how to ask it.
struct CardInput {
  const core::pack::Pack* pack = nullptr;
  core::pack::Item item{};
  uint32_t index = 0;  // catalog index
  core::session::Format format = core::session::Format::Flashcard;
  uint8_t reps = 0;  // grades so far: seeds the options and the tiles
  TextSize size = TextSize::Medium;
  bool touch = false;
  bool showVulgar = false;
};

// How an auto-graded card was answered, for the usage log.
struct AnswerDetail {
  uint8_t chosen = 0xFF;        // the option index; 0xFF none
  uint8_t attempts = 0;         // wrong tiles before the sentence was built
  const char* typed = nullptr;  // what was typed; null when nothing was
};

// Action ids a view registers for its own touch targets (options, tiles,
// keyboard keys) start here; SessionScreen passes them on.
inline constexpr freeink::ui::ActionId kExerciseAction = app::kFirstViewAction + 32;

class ExerciseView {
 public:
  enum class Reply : uint8_t {
    Ignored,   // not this view's input
    Handled,   // consumed; the card changed (repaint)
    Answered,  // the item is answered: `grade` is set
  };

  virtual ~ExerciseView() = default;

  // Whether this view asks this item in the card's format.
  virtual bool accepts(const CardInput& card) const = 0;
  // A new card (or the same one again after an undo); before any draw().
  // False when the item cannot be asked this way after all (too few
  // options): the controller then falls back to a flashcard.
  virtual bool load(const CardInput& card) = 0;
  // The code journalled with the grade.
  virtual ExerciseFormat journalFormat() const = 0;
  // Draws the front, or the back, into `area`, and registers the view's touch
  // targets (front only). Nothing may go below `area`. The back keeps the
  // front's layout so that turning the card only adds ink (a fast refresh
  // leaves no ghost), unless resultIsNewScreen().
  virtual void draw(app::UiScreen& screen, freeink::ui::Rect area, bool back) = 0;

  virtual bool autoGraded() const { return false; }
  // Auto-graded: the answers over the footer cells on the front, or null when
  // the view takes its answer another way (tiles, a keyboard). The result
  // keeps the same footer, so that it too only adds ink.
  virtual const ChoiceBar* answers() const { return nullptr; }
  // The footer's key hints on key devices when answers() is null.
  virtual bool keyHints(const KeyMap& keys, ChoiceBar& out) const {
    (void)keys;
    (void)out;
    return false;
  }
  // The result removes ink the front had (a keyboard): a new screen's refresh.
  virtual bool resultIsNewScreen() const { return false; }
  // For the usage log: the options as shown and the right one's index (0xFF
  // none), returning how many; and how the card was answered.
  virtual uint8_t optionTexts(const char** out, uint8_t cap, uint8_t& answer) const {
    (void)out;
    (void)cap;
    answer = 0xFF;
    return 0;
  }
  virtual AnswerDetail answerDetail() const { return AnswerDetail{}; }

  // Whether an auto-graded result says how to go on; off under the leech
  // notice, whose own choices go on.
  void showGoOn(bool on) { goOnHint_ = on; }
  bool goOnHint() const { return goOnHint_; }

  // Front input of auto-graded views: a footer cell, a raw key, or one of the
  // view's own touch targets.
  virtual Reply onChoice(uint8_t cell, core::Grade& grade) {
    (void)cell;
    (void)grade;
    return Reply::Ignored;
  }
  virtual Reply onKey(const InputEvent& event, core::Grade& grade) {
    (void)event;
    (void)grade;
    return Reply::Ignored;
  }
  virtual Reply onAction(const app::ActionEvent& event, core::Grade& grade) {
    (void)event;
    (void)grade;
    return Reply::Ignored;
  }

 private:
  bool goOnHint_ = true;
};

// The views, in the order SessionController tries them: the first that
// accepts the picked format asks the item. They exist from App::open() to
// App's end (openExerciseViews() false: out of memory).
bool openExerciseViews();
void closeExerciseViews();
ExerciseView* const* exerciseViews(uint8_t& count);

}  // namespace tinta::ui

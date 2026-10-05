#pragma once

// Readings (PLAN.md 4.5 reader, M6).
//
// ReadingsScreen  the course's readings (stories of kind reading; lesson
//                 dialogues are read in their lessons), each with its level and
//                 a check once read. A reading opens when the lesson it belongs
//                 to is current, done or unlocked; until then it is locked.
// ReaderScreen    the reading a page at a time, typeset a paragraph at a time
//                 (core/reader), page starts kept in a small array. Every word
//                 the dictionary has can be glossed: the gloss box shows the
//                 headword with its article, respelling, gloss, whether it is
//                 in the deck (and adds it), and the English of the sentence.
//                   Key devices: Left/Right move the word cursor (an underline),
//                   side Up/Down turn pages, Confirm opens the gloss; in the
//                   gloss Confirm adds the word to the deck (or takes it out),
//                   Back closes it.
//                   X4 Pro: tap a word; tap the star to add it; tap anywhere
//                   else to close; the footer's English shows the sentence of
//                   the last word tapped (the page's first sentence before
//                   that); arrows or a swipe turn pages.
//                 After the last page, the questions.
// QuizScreen      the reading's comprehension questions as choices (answered
//                 like an exercise: one press, the result marked, any key
//                 goes on), then the score. Finishing marks the reading read.
//                 A reading without questions is read once its last page has
//                 been shown.
//
// Simulator logs: "[tinta] reader <story> page <p>/<n>", "[tinta] reader word
// <token> <word>" as the cursor moves, "[tinta] gloss <headword> <state>",
// targets "word/<token>", "gloss/star"; "[tinta] quiz <q>/<n> options <k>
// right <i>", "[tinta] quiz <q>/<n> answer <i> right|wrong", "[tinta] quiz
// <right>/<n> done", "[tinta] read <story>".

#include "app/View.h"
#include "core/pack/Pack.h"
#include "core/reader/Reading.h"
#include "ui/views/ListView.h"

namespace tinta::ui {

// Opens ReaderScreen on story `story`.
void showReading(app::App& app, uint16_t story);

class ReadingsScreen final : public ListView {
 public:
  using ListView::ListView;

  const char* name() const override { return "readings"; }
  const char* title() const override;
  void enter(bool returning) override;

 protected:
  uint16_t rowCount() const override { return count_; }
  void drawRow(app::UiScreen& screen, freeink::ui::Rect row, uint16_t index, bool focused) override;
  void activate(uint16_t index) override;
  bool selectable(uint16_t index) const override;
  int16_t rowHeight() const override;

 private:
  static constexpr uint16_t kMax = 64;
  uint16_t stories_[kMax] = {};
  uint16_t count_ = 0;
};

class ReaderScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "reader"; }
  const char* title() const override;
  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  void onBack() override;
  const ChoiceBar* choiceBar() const override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override { return -1; }
  // The story is the list's choice: not kept across a wake.
  bool restorable() const override { return false; }

 private:
  struct PageStart {
    uint8_t paragraph;
    uint16_t span;
    uint16_t offset;
  };
  // A word drawn on the page: its number in the story and where it is (a
  // word broken over two lines has two entries).
  struct WordBox {
    uint16_t token;
    int16_t x, y, w, h;
  };
  static constexpr uint8_t kMaxParagraphs = 32;
  static constexpr uint8_t kMaxPages = 48;
  static constexpr uint8_t kMaxWords = 160;

  void paginate(freeink::ui::Rect area);
  uint16_t loadParagraph(uint8_t paragraph);
  void drawPage(app::UiScreen& screen, freeink::ui::Rect area);
  void drawGloss(app::UiScreen& screen, freeink::ui::Rect area);
  void drawEnglish(app::UiScreen& screen, freeink::ui::Rect area);
  bool glossable(uint16_t token) const;
  uint16_t lemmaOf(uint16_t token, uint16_t* sentence = nullptr) const;
  void moveCursor(int8_t dir);
  void turn(int8_t dir);
  void openGloss(uint16_t token);
  void closeGloss();
  void star();
  void toEnd();
  void logPage() const;

  uint16_t story_ = 0;
  core::pack::Story record_{};
  uint32_t key_ = 0;     // library::storyKey, for the usage log
  bool opened_ = false;  // its opening recorded once the pages are known
  core::reader::Paragraph paragraphs_[kMaxParagraphs] = {};
  uint8_t paragraphCount_ = 0;
  PageStart pages_[kMaxPages] = {};
  uint8_t pageCount_ = 0;
  uint8_t page_ = 0;
  int16_t laidOutHeight_ = -1;
  WordBox words_[kMaxWords] = {};
  uint8_t wordCount_ = 0;
  int32_t cursor_ = -1;        // token under the key cursor, or -1
  int32_t glossToken_ = -1;    // the word the gloss box shows, or -1
  int32_t englishToken_ = -1;  // touch: the sentence whose English shows, or -1
  int32_t lastTapped_ = -1;
  bool cursorToEnd_ = false;  // after a turn back: the cursor goes to the last word
  bool traceNext_ = false;
  freeink::ui::Rect body_{};      // where taps are this screen's own
  freeink::ui::Rect starRect_{};  // the gloss's star, while it shows one
  ChoiceBar bar_{};
  mutable char title_[48] = {};
};

class QuizScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "quiz"; }
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
  void choose(uint8_t option);
  void goOn();
  void logQuestion() const;

  uint16_t story_ = 0;
  core::pack::Story record_{};
  uint32_t key_ = 0;  // library::storyKey, for the usage log
  uint8_t question_ = 0;
  int8_t chosen_ = -1;
  uint8_t right_ = 0;
  bool done_ = false;
  bool traceNext_ = false;
  ChoiceBar bar_{};
  mutable char cells_[4][4] = {};
};

}  // namespace tinta::ui

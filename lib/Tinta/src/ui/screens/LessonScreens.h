#pragma once

// Lessons (PLAN.md 4.1, M5).
//
// LessonScreen  one lesson, a page at a time: the cover (title, what the
//               lesson holds), its notes (styled text, as many pages as each
//               needs), the dialogue (speaker lines; Confirm or the footer's
//               English shows each line's English under it), a presentation
//               card per new word, and the practice page, whose Start runs
//               the lesson's practice (core/session/Lessons.h) as a session.
//               Finishing that practice completes the lesson and unlocks the
//               next (App::lessonCompleted). Key devices: Left/Right (and the
//               side keys) turn pages; touch: the footer's arrows or a swipe.
// CourseScreen  the course map: every unit and its lessons, each done,
//               current, open (unlocked ahead) or locked. A lesson that is not
//               locked opens in LessonScreen, so a finished lesson's notes and
//               dialogue can be re-read and practised again.
//
// Simulator logs: "[tinta] lesson <n> page <p>/<count> <kind> <index>" on
// every page; "[tinta] course cursor <lesson> <state>" as the key cursor
// moves; touch targets "lesson/start", "course/l<lesson>".

#include "app/View.h"
#include "core/text/Typesetter.h"
#include "ui/views/Cards.h"

namespace tinta::ui {

// Opens LessonScreen on `lesson` (a pack lesson index).
void showLesson(app::App& app, uint16_t lesson);

class LessonScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "lesson"; }
  const char* title() const override;
  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  const ChoiceBar* choiceBar() const override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override { return -1; }
  // Which lesson is the Home row's or the map's choice: not kept across a
  // wake (Home is a press away).
  bool restorable() const override { return false; }

 private:
  enum class PageKind : uint8_t { Cover, Note, Dialogue, Word, Practice };
  struct Page {
    PageKind kind;
    uint8_t index;  // note, or word
    uint16_t at;    // note: span; dialogue: first line
    uint16_t offset;
  };
  static constexpr uint8_t kMaxPages = 64;
  static constexpr uint8_t kMaxWords = 32;

  void paginate(freeink::ui::Rect area);
  void addPage(PageKind kind, uint8_t index, uint16_t at = 0, uint16_t offset = 0);
  uint16_t loadNote(uint8_t note);
  int16_t noteHead(freeink::ui::DisplayTarget& t, Column& col, uint8_t note);
  int16_t dialogueHead(freeink::ui::DisplayTarget& t, Column& col);
  uint16_t dialogueLines(freeink::ui::DisplayTarget& t, Column& col, uint16_t first);
  void drawCover(freeink::ui::DisplayTarget& t, Column& col);
  void drawNote(freeink::ui::DisplayTarget& t, Column& col, const Page& page);
  void drawPractice(app::UiScreen& screen, Column& col);
  void turn(int8_t dir);
  void toggleEnglish();
  void startPractice();
  void logPage() const;
  bool onPractice() const;
  bool onDialogue() const;

  uint16_t lesson_ = 0;
  Page pages_[kMaxPages] = {};
  uint8_t pageCount_ = 0;
  uint8_t page_ = 0;
  int16_t laidOutHeight_ = -1;
  bool english_ = false;
  int32_t pendingLine_ = -1;  // after the English toggles: the line to stay on
  bool traceNext_ = false;
  uint8_t wordCount_ = 0;
  uint32_t words_[kMaxWords] = {};  // the new words' recognise items
  uint16_t practiceCount_ = 0;
  FlashcardView present_;
  ChoiceBar bar_{};
  mutable char title_[32] = {};
};

class CourseScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "course"; }
  const char* title() const override;
  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  const ChoiceBar* choiceBar() const override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override { return -1; }

 private:
  // Row r: a unit heading (lesson < 0) or a lesson.
  struct Row {
    int32_t lesson;
    uint16_t unit;
  };
  uint16_t rowCount() const;
  bool rowAt(uint16_t r, Row& out) const;
  uint16_t rowOfLesson(uint16_t lesson) const;
  void moveCursor(int8_t dir);
  void turnPage(int8_t dir);
  void open(uint16_t lesson);
  void logCursor() const;

  uint16_t top_ = 0;
  uint16_t cursor_ = 0;  // a lesson row (key devices)
  uint8_t rows_ = 1;
  bool placeCursor_ = false;
  bool traceNext_ = false;
  ChoiceBar bar_{};
};

}  // namespace tinta::ui

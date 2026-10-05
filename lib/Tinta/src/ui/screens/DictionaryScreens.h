#pragma once

// The dictionary, straight from the pack (PLAN.md 8.5, M3, M6). The X4 Pro
// also searches with its keyboard (SearchScreen).
//
// DictionaryScreen  every headword in alphabetical order (LKEY order), a page
//                   at a time, drawn from the pack as it scrolls: nothing is
//                   copied to RAM. Key devices: Up/Down move the cursor (and
//                   turn pages at the edges), Right turns a page, Left opens
//                   the letters, Confirm opens the entry. Touch: tap a row;
//                   the footer has the letters and page up/down.
// LetterSheet       A-Z in a grid; letters without words are grey. On key
//                   devices it builds a prefix a letter at a time and switches
//                   the list between Spanish and English (EKEY).
// EntryScreen       headword, part of speech and gender, respelling, glosses,
//                   forms, usage note and examples, paginated in whole
//                   blocks; a verb's Conjugation opens its table.
// VerbTableScreen   one tense per page, five rows (no vosotros).

#include "app/View.h"
#include "core/usage/UsageLog.h"
#include "ui/views/FormView.h"

namespace tinta::ui {

// Opens the dictionary entry of `lemma` (from the reader, search, ...).
void showEntry(app::App& app, uint16_t lemma, core::usage::Source via);

class DictionaryScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "dictionary"; }
  const char* title() const override;
  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  const ChoiceBar* choiceBar() const override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override { return -1; }

  // The letter sheet's choice: show the list from index `index` (of LKEY, or
  // EKEY in English).
  void jumpTo(uint32_t index);
  uint32_t top() const { return top_; }
  // English -> Spanish: the list runs over the English keys.
  void setEnglish(bool english);

 private:
  uint32_t total() const;
  void moveCursor(int32_t delta);
  void turnPage(int32_t pages);
  void openEntry(uint32_t index);
  void logFocus() const;

  uint32_t top_ = 0;     // first row on the page
  uint32_t cursor_ = 0;  // key devices: the focused row
  uint8_t rows_ = 1;     // rows per page, from the last build
  bool traceNext_ = false;
  ChoiceBar bar_{};
};

class LetterSheet final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "dict-letters"; }
  Kind kind() const override { return Kind::Overlay; }
  bool restorable() const override { return false; }
  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override { return -1; }

 private:
  static constexpr uint8_t kLetters = 26;
  static constexpr uint8_t kSpaceCell = 26;
  static constexpr uint8_t kDeleteCell = 27;
  static constexpr uint8_t kModeCell = 28;
  static constexpr uint8_t kCells = 29;
  static constexpr uint8_t kColumns = 7;
  void pick(uint8_t cell);
  void refresh();
  bool cellEnabled(uint8_t cell) const;
  // Key devices: the cursor steps over enabled cells only, wrapping.
  void step(int8_t by);

  char prefix_[12] = {};
  uint8_t length_ = 0;
  uint32_t next_ = 0;  // bit i: letter 'a' + i can follow the prefix
  uint8_t focus_ = 0;
  bool traceNext_ = false;
};

class EntryScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "dict-entry"; }
  const char* title() const override;
  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  const ChoiceBar* choiceBar() const override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override { return -1; }
  // The lemma is the list's choice, not kept across a wake.
  bool restorable() const override { return false; }

 private:
  static constexpr uint8_t kMaxPages = 12;
  bool isVerb() const;
  void turn(int8_t dir);
  void openTable();
  void confirm();
  void toggleStar();

  uint8_t page_ = 0;
  uint8_t pageCount_ = 1;
  uint8_t pageStart_[kMaxPages + 1] = {};
  int16_t laidOutHeight_ = -1;
  bool traceNext_ = false;
  ChoiceBar bar_{};
};

// Key devices, a verb that can be starred: Conjugation or Add to my deck.
class EntryActionsSheet final : public FormView {
 public:
  using FormView::FormView;

  const char* name() const override { return "entry-actions"; }
  Kind kind() const override { return Kind::Overlay; }
  bool restorable() const override { return false; }
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;

 protected:
  uint8_t rowCount() const override;
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void activate(uint8_t index) override;
};

class VerbTableScreen final : public app::View {
 public:
  using View::View;

  const char* name() const override { return "verb-table"; }
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
  void turn(int8_t dir);

  uint8_t tense_ = 0;
  ChoiceBar bar_{};
};

}  // namespace tinta::ui

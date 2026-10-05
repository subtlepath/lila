#pragma once

// A screen that is a list of rows longer than a FormView holds (the readings,
// the phrasebook's categories): a page of rows at a time, drawn by the
// subclass.
//
//   Key devices  Up/Down (side keys) move the cursor and turn pages at the
//                edges; Left/Right turn a page; Confirm opens the row.
//   X4 Pro       tap a row; the footer's arrows turn pages; swipe up/down.
//
// Rows that are not selectable (locked) are drawn but skipped by the cursor
// and get no touch target. Simulator logs: "[tinta] list <name> cursor <i>"
// as the cursor moves, "[tinta] target <name>/row<i> x y" for each row on a
// touch page (i counts from the first row of the list).

#include "app/View.h"

namespace tinta::ui {

class ListView : public app::View {
 public:
  using View::View;

  void enter(bool returning) override;
  void build(app::UiScreen& screen) override;
  void onAction(const app::ActionEvent& event) override;
  bool onInput(const InputEvent& event) override;
  const ChoiceBar* choiceBar() const override;
  bool keyHints(ChoiceBar& out) const override;
  int8_t focusOrdinal() const override { return -1; }

 protected:
  virtual uint16_t rowCount() const = 0;
  virtual void drawRow(app::UiScreen& screen, freeink::ui::Rect row, uint16_t index, bool focused) = 0;
  virtual void activate(uint16_t index) = 0;
  virtual bool selectable(uint16_t index) const {
    (void)index;
    return true;
  }
  virtual int16_t rowHeight() const;
  // Above the rows (a note, a count); default none.
  virtual void buildHeader(app::UiScreen& screen) { (void)screen; }
  // The row the cursor starts on when the screen opens.
  virtual uint16_t initialRow() const { return 0; }

  uint16_t cursor() const { return cursor_; }

 private:
  void move(int8_t dir);
  void turnPage(int8_t dir);
  void logCursor() const;

  uint16_t top_ = 0;
  uint16_t cursor_ = 0;
  uint8_t rows_ = 1;
  bool placeCursor_ = false;
  bool traceNext_ = false;
  ChoiceBar bar_{};
};

}  // namespace tinta::ui

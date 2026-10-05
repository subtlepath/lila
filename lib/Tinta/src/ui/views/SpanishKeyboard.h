#pragma once

// The X4 Pro's Spanish keyboard (typed answers, dictionary search): FreeInkUI's
// keyboard() with a layout of Tinta's own, because the SDK's SpanishEs layout
// has ñ but no accented vowels and its long press is awkward on e-paper. A row
// of á é í ó ú ü ñ sits above the letters; Delete and OK (labelled by the
// caller) as usual.
//
// In simulator builds draw() can log the centre of every key once, as
// "[tinta] target key/<label> x y" (labels: the letter, "del", "space",
// "OK"), so flows can type.

#include <stddef.h>
#include <stdint.h>

#include "app/View.h"

namespace tinta::ui {

class SpanishKeyboard {
 public:
  // Three action ids in a row from `first`: a key, Delete, OK.
  explicit SpanishKeyboard(app::ActionId first) : first_(first) {}

  // Height for the five rows at the size typing needs.
  static int16_t height();

  // Draws into `rect` (normally the full width) and registers the keys.
  void draw(app::UiScreen& screen, freeink::ui::Rect rect, const char* okLabel);
  // The next draw() logs the key centres again (a new card, a new screen).
  void traceNext() { trace_ = true; }

  enum class Edit : uint8_t { None, Changed, Ok };
  // Applies a key, Delete or OK to `text` (UTF-8, NUL-terminated, `cap`
  // bytes); None when the event is not this keyboard's.
  Edit apply(const app::ActionEvent& event, char* text, size_t cap) const;

 private:
  app::ActionId first_;
  bool trace_ = true;
};

}  // namespace tinta::ui

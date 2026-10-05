#pragma once

// Drawing pieces shared by the exercise views, the dictionary and the leech
// notice: every piece of mixed or Spanish text goes through the typesetter
// (PLAN.md 5.5). Each call lays out into the rect it is given, draws, and
// returns the height it used (0 when nothing fitted), so a card is a column
// of calls that stops at the bottom of its area and never draws past it.
//
// The typesetter's runs live in one static buffer: lay out and draw one
// block at a time, on the loop task.

#include <FreeInkUIDisplayTarget.h>
#include <stddef.h>
#include <stdint.h>

#include "app/View.h"
#include "core/pack/Pack.h"
#include "ui/Fonts.h"

namespace tinta::ui {

// The screen's target as the DisplayTarget it is: the typesetter re-points
// its scratch font slot, which only DisplayTarget offers. Every frame Tinta
// builds draws into App's one DisplayTarget.
inline freeink::ui::DisplayTarget& displayOf(app::UiScreen& screen) {
  return static_cast<freeink::ui::DisplayTarget&>(screen.target());
}

// A column being filled from the top: x, width and the bottom are fixed; y
// moves down. Every draw call below takes it, draws at `y` if the block fits
// above `bottom`, and advances `y` past it.
struct Column {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t bottom = 0;
  // Lay out and advance without drawing (to paginate).
  bool dryRun = false;
  // A block that does not fit whole is not drawn at all (pages); otherwise
  // the lines that fit are drawn (cards).
  bool whole = false;

  int16_t room() const { return static_cast<int16_t>(bottom > y ? bottom - y : 0); }
  void skip(int16_t pixels) { y = static_cast<int16_t>(y + pixels); }
};

// "NOUN · f · informal · MX" in the label font (chrome language).
// The one layout buffer every card shares (about 1.4 KB), from App::open()
// to App's end; false when out of memory.
bool openCardText();
void closeCardText();

void formatLemmaLabel(const core::pack::Lemma& lemma, char* out, size_t cap);
// "la chamba", "el agua", "los lentes", "el/la estudiante"; the headword alone
// for anything but a noun.
void formatHeadword(const core::pack::Pack& pack, const core::pack::Lemma& lemma, bool withArticle, char* out,
                    size_t cap);

// One line in `role` at the profile's text size; wraps up to `maxLines`
// (0: as many as fit). False when not even one line fitted.
bool drawText(freeink::ui::DisplayTarget& t, Column& col, const char* text, FontRole role, TextSize size,
              uint8_t maxLines = 0, int16_t gapAfter = 0);
// The same with an explicit font.
bool drawTextIn(freeink::ui::DisplayTarget& t, Column& col, const char* text, const BitmapFont& font,
                uint8_t maxLines = 0, int16_t gapAfter = 0);

// The headword in the largest display strike that fits the width.
bool drawHeadword(freeink::ui::DisplayTarget& t, Column& col, const char* text, int16_t gapAfter = 0);

// A pack sentence in SpanishText. Tokens of `lemma` (or, when `lemma` is
// kNone16, the token at `tokenIndex`) are set in SpanishEmphasis; with
// `blank` they are drawn as a gap of the same width instead.
bool drawSentence(freeink::ui::DisplayTarget& t, Column& col, const core::pack::Pack& pack,
                  const core::pack::Sentence& sentence, uint16_t lemma, int16_t tokenIndex, bool blank, TextSize size,
                  int16_t gapAfter = 0);

// "Gerund: hablando": an English label (EnglishTranslation) and Spanish
// (SpanishText) on one baseline. False for an empty value.
bool drawLabelled(freeink::ui::DisplayTarget& t, Column& col, const char* label, const char* value, TextSize size,
                  int16_t gapAfter = 0);

// A thin horizontal rule across the column.
void drawRule(freeink::ui::DisplayTarget& t, Column& col, int16_t gapAfter);

// "1.2": a lesson's unit number and its number in the unit.
void lessonCode(const core::pack::Pack& pack, uint16_t lesson, char* out, size_t cap);

// Tense names in the chrome language ("present" / "presente"), or always in
// Spanish.
const char* tenseName(core::pack::Tense tense);
const char* tenseNameSpanish(core::pack::Tense tense);
// The Spanish subject for a verb-table row: "yo", "tú", "él / ella / usted",
// "nosotros", "ellos / ellas / ustedes".
const char* personLabel(core::pack::Person person);

}  // namespace tinta::ui

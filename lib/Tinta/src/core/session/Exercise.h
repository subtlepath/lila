#pragma once

#include <cstdint>

#include "core/lang/AnswerCheck.h"
#include "core/pack/Pack.h"
#include "core/srs/Fsrs.h"
#include "core/srs/ItemState.h"

namespace tinta::core::session {

// How an item is asked this time (PLAN.md 4.1, 4.3, 8.2 step 4). The item is
// what is learnt; the format is chosen at review time from the kind, how well
// the item is known and the device, deterministically from the uid and the
// repetition count, so a resumed card is asked the same way.
enum class Format : uint8_t {
  Flashcard,      // self-graded: reveal, then four grades
  ChooseMeaning,  // recognise: the Spanish, English options
  ChooseWord,     // produce: the English, Spanish options
  ChooseGap,      // cloze: the sentence with a gap and its English; options fill the gap
  ChooseArticle,  // gender: el or la (los or las for a plural-only noun)
  ChooseForm,     // conjugation: the infinitive, person and tense; forms as options
  BuildSentence,  // word order: the English and the words as tiles
  TypeWord,       // produce, typed (touch devices, a setting)
  TypeGap,        // cloze, typed
  TypeForm,       // conjugation, typed
};

bool isChoice(Format f);
bool isTyped(Format f);

// What the picker knows of an item's history.
struct History {
  uint8_t reps = 0;         // grades so far
  float stabilityDays = 0;  // 0 for a new item
  bool isNew = true;
};

// Items with stability of this many days are mature: flashcards from then on.
inline constexpr float kMatureDays = 21.0f;

inline History historyOf(const ItemState& state) {
  History h;
  h.reps = state.reps;
  h.isNew = state.isNew();
  h.stabilityDays = state.isNew() ? 0.0f : state.stabilityDays();
  return h;
}

// `typing`: the learner turned typed answers on, on a device with a touch
// keyboard. A choice format may still turn out unusable (too few options
// once vulgar or duplicate candidates are dropped); the caller then falls
// back to Flashcard.
Format pickFormat(const pack::Item& item, uint32_t uid, const History& history, bool typing);

// What the session asks: pickFormat(), or Flashcard when the item cannot be
// asked that way after all (a choice with fewer than two usable options, a
// sentence that does not make tiles, a typed format with nothing to type).
// The exercise views make the same checks when they load.
Format resolveFormat(const pack::Pack& pack, const pack::Item& item, const History& history, bool typing,
                     bool showVulgar);

// True when the item practises a word or sentence tagged vulgar: such items
// are never introduced or shown while "show vulgar words" is off.
bool vulgarItem(const pack::Pack& pack, const pack::Item& item);

// ---- Options -----------------------------------------------------------------

enum class OptionKind : uint8_t {
  Lemma,    // a lemma id: its English (ChooseMeaning) or its Spanish (ChooseWord)
  String,   // a pack string offset (a candidate of a cloze or conjugation item)
  Token,    // the item's own sentence token (the right answer of a cloze)
  Form,     // a verb-form tag of the item's verb (the right answer of a conjugation)
  Article,  // 1 el / los, 2 la / las
};

struct Option {
  OptionKind kind = OptionKind::Lemma;
  uint32_t value = 0;
};

struct OptionSet {
  static constexpr uint8_t kMax = 4;
  Option options[kMax];
  uint8_t count = 0;   // 2..4 when usable
  uint8_t answer = 0;  // index of the right option
};

// The options for a choice format: the answer and up to three distractors
// from the item's candidate list (docs/pack-format.md 3.9). Authored
// confusable partners (the leading flags & 3 candidates) are always among
// them; the rest are drawn from the best-ranked candidates with a generator
// seeded by uid and `reps`, so the same repetition shows the same options.
// Distractors that read the same as an option already taken, dictionary-only
// lemmas and, unless `showVulgar`, vulgar lemmas are skipped. Shuffled with
// the same seed. False (count < 2) when nothing usable is left.
bool pickOptions(const pack::Pack& pack, const pack::Item& item, Format format, uint32_t uid, uint8_t reps,
                 bool showVulgar, OptionSet& out);

// An option as shown, NUL-terminated in `out` (at most cap - 1 bytes).
void optionText(const pack::Pack& pack, const pack::Item& item, Format format, const Option& option, char* out,
                size_t cap);

// The expected answer of a typed format, for lang::checkAnswer.
void typedAnswer(const pack::Pack& pack, const pack::Item& item, Format format, char* out, size_t cap);

// ---- Word order --------------------------------------------------------------

// The tiles of a word-order item: the sentence's words, shuffled. The learner
// picks them in order; a word that is not the next one is counted as a
// mistake and stays on the tray. Words that read the same are
// interchangeable.
class WordOrder {
 public:
  static constexpr uint8_t kMaxTiles = 12;
  // The sentence is copied: tiles outlive the pass that read it.
  static constexpr uint8_t kMaxText = 128;

  // False when the sentence has fewer than two or more than kMaxTiles words.
  bool begin(const pack::Pack& pack, const pack::Sentence& sentence, uint32_t seed);

  uint8_t tileCount() const { return count_; }
  // Tile i in tray order (shuffled).
  pack::TextSpan tile(uint8_t i) const;
  bool placed(uint8_t i) const { return i < count_ && (placedMask_ >> i) & 1u; }
  uint8_t placedCount() const { return next_; }
  // The tile placed k-th (k < placedCount()).
  uint8_t placedAt(uint8_t k) const { return k < next_ ? order_[k] : 0; }
  // Picks tile i; true when it was the next word (it is placed).
  bool pick(uint8_t i);
  bool done() const { return count_ > 0 && next_ == count_; }
  uint8_t mistakes() const { return mistakes_; }

 private:
  bool sameText(uint8_t tile, uint8_t word) const;

  char text_[kMaxText] = {};
  uint8_t start_[kMaxTiles] = {};  // in sentence order, into text_
  uint8_t length_[kMaxTiles] = {};
  uint8_t tray_[kMaxTiles] = {};   // tray slot -> word index
  uint8_t order_[kMaxTiles] = {};  // placement order -> tray slot
  uint8_t count_ = 0;
  uint8_t next_ = 0;
  uint8_t mistakes_ = 0;
  uint16_t placedMask_ = 0;
};

// ---- Grades ------------------------------------------------------------------

// PLAN.md 4.3: wrong -> Again; right after a second try or a hint -> Hard;
// right -> Good.
inline Grade gradeChoice(bool right) { return right ? Grade::Good : Grade::Again; }
inline Grade gradeTyped(lang::Verdict v) {
  return !lang::accepted(v) ? Grade::Again : lang::shaky(v) ? Grade::Hard : Grade::Good;
}
inline Grade gradeTiles(uint8_t mistakes) {
  return mistakes == 0 ? Grade::Good : mistakes == 1 ? Grade::Hard : Grade::Again;
}

// The seed of the deterministic choices for one showing of an item.
uint32_t showingSeed(uint32_t uid, uint8_t reps);

}  // namespace tinta::core::session

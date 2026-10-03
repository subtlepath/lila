#include "BookSearchText.h"

#include <Utf8.h>

#include <cstring>

// Read at compile time only, to build LATIN_FOLD below; no copy of the table is emitted here.
#include "Utf8ComposeTable.h"

namespace booksearch {

namespace {

constexpr uint32_t LATIN_FIRST = 0x00C0;
constexpr uint32_t LATIN_LAST = 0x024F;
constexpr uint32_t LATIN_ADDITIONAL_FIRST = 0x1E00;
constexpr uint32_t LATIN_ADDITIONAL_LAST = 0x1EFF;
constexpr size_t LATIN_SIZE = LATIN_LAST - LATIN_FIRST + 1;
constexpr size_t LATIN_FOLD_SIZE = LATIN_SIZE + (LATIN_ADDITIONAL_LAST - LATIN_ADDITIONAL_FIRST + 1);

constexpr int latinSlot(const uint32_t cp) {
  if (cp >= LATIN_FIRST && cp <= LATIN_LAST) return static_cast<int>(cp - LATIN_FIRST);
  if (cp >= LATIN_ADDITIONAL_FIRST && cp <= LATIN_ADDITIONAL_LAST) {
    return static_cast<int>(LATIN_SIZE + cp - LATIN_ADDITIONAL_FIRST);
  }
  return -1;
}

constexpr uint16_t asciiLower(const uint32_t cp) {
  return static_cast<uint16_t>(cp >= 'A' && cp <= 'Z' ? cp - 'A' + 'a' : cp);
}

struct LatinFoldTable {
  // Folded letter for each Latin codepoint; 0 = not a single letter (see MULTI_LETTER_FOLDS) or unchanged.
  uint16_t letters[LATIN_FOLD_SIZE] = {};
};

// Base letters for the Latin and Latin Extended Additional blocks, derived from the NFC table at compile
// time: a precomposed letter folds to its fully decomposed base (ṝ -> ṛ -> r), lowercased. Letters with no
// decomposition are mapped by hand.
constexpr LatinFoldTable buildLatinFold() {
  LatinFoldTable table;
  for (const auto& entry : kUtf8ComposeTable) {
    const int slot = latinSlot(entry.composed);
    if (slot >= 0) table.letters[slot] = entry.base;
  }
  for (int pass = 0; pass < 3; pass++) {
    for (auto& letter : table.letters) {
      const int slot = latinSlot(letter);
      if (slot >= 0 && table.letters[slot] != 0) letter = table.letters[slot];
    }
  }
  struct Explicit {
    uint16_t cp;
    char letter;
  };
  constexpr Explicit EXPLICIT[] = {
      {0x00D0, 'd'}, {0x00F0, 'd'}, {0x00D8, 'o'}, {0x00F8, 'o'}, {0x0110, 'd'}, {0x0111, 'd'}, {0x0126, 'h'},
      {0x0127, 'h'}, {0x0131, 'i'}, {0x013F, 'l'}, {0x0140, 'l'}, {0x0141, 'l'}, {0x0142, 'l'}, {0x0166, 't'},
      {0x0167, 't'}, {0x017F, 's'}, {0x0180, 'b'}, {0x0192, 'f'}, {0x01FE, 'o'}, {0x01FF, 'o'}, {0x0237, 'j'},
  };
  for (const auto& e : EXPLICIT) table.letters[latinSlot(e.cp)] = static_cast<uint16_t>(e.letter);
  for (auto& letter : table.letters) letter = asciiLower(letter);
  return table;
}

constexpr LatinFoldTable LATIN_FOLD = buildLatinFold();

struct MultiFold {
  uint16_t cp;
  const char* letters;
};

// Letters and ligatures that fold to more than one letter.
constexpr MultiFold MULTI_LETTER_FOLDS[] = {
    {0x00C6, "ae"}, {0x00E6, "ae"}, {0x01E2, "ae"}, {0x01E3, "ae"},  {0x01FC, "ae"},  {0x01FD, "ae"}, {0x0152, "oe"},
    {0x0153, "oe"}, {0x00DF, "ss"}, {0x1E9E, "ss"}, {0x00DE, "th"},  {0x00FE, "th"},  {0x0132, "ij"}, {0x0133, "ij"},
    {0xFB00, "ff"}, {0xFB01, "fi"}, {0xFB02, "fl"}, {0xFB03, "ffi"}, {0xFB04, "ffl"}, {0xFB05, "st"}, {0xFB06, "st"},
};

Folded letters(const uint32_t unit) {
  Folded f;
  f.kind = Folded::Letters;
  f.count = 1;
  f.units[0] = unit;
  return f;
}

Folded ofKind(const Folded::Kind kind) {
  Folded f;
  f.kind = kind;
  return f;
}

bool isIgnorable(const uint32_t cp) {
  return cp == 0x00AD                       // soft hyphen
         || cp == 0x034F                    // combining grapheme joiner
         || (cp >= 0x200B && cp <= 0x200F)  // zero-width space/joiners, LRM, RLM
         || (cp >= 0x202A && cp <= 0x202E)  // bidi embedding controls
         || (cp >= 0x2060 && cp <= 0x206F)  // word joiner, invisible operators, bidi isolates
         || cp == 0xFEFF                    // byte-order mark / zero-width no-break space
         || utf8IsCombiningMark(cp)         //
         || (cp >= 0x0591 && cp <= 0x05C7 && cp != 0x05BE && cp != 0x05C0 && cp != 0x05C3 &&
             cp != 0x05C6)                  // Hebrew points
         || (cp >= 0x0610 && cp <= 0x061A)  // Arabic marks
         || (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 ||
         (cp >= 0x06D6 && cp <= 0x06ED && cp != 0x06DD && cp != 0x06DE && cp != 0x06E5 && cp != 0x06E6 && cp != 0x06E9);
}

bool isPunctuationOrSpace(const uint32_t cp) {
  if (cp < 0x80) return !((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'));
  if (cp < 0xC0) return cp != 0xAA && cp != 0xB5 && cp != 0xBA;  // Latin-1 punctuation; ª µ º are letters
  if (cp == 0xD7 || cp == 0xF7) return true;                     // × ÷
  return cp == 0x037E || cp == 0x0387                            // Greek question mark, ano teleia
         || (cp >= 0x055A && cp <= 0x055F) || cp == 0x0589       // Armenian
         || cp == 0x05BE || cp == 0x05C0 || cp == 0x05C3 || cp == 0x05C6 || cp == 0x05F3 || cp == 0x05F4    // Hebrew
         || cp == 0x060C || cp == 0x061B || cp == 0x061F || (cp >= 0x066A && cp <= 0x066D) || cp == 0x06D4  // Arabic
         || cp == 0x0964 || cp == 0x0965                  // Devanagari danda
         || cp == 0x0E2F || cp == 0x0E5A || cp == 0x0E5B  // Thai
         || (cp >= 0x2000 && cp <= 0x2BFF)                // spaces, punctuation, symbols, arrows, shapes
         || (cp >= 0x2E00 && cp <= 0x2E7F)                // supplemental punctuation
         || (cp >= 0x3000 && cp <= 0x3004) || (cp >= 0x3008 && cp <= 0x3020) || cp == 0x3030 || cp == 0x30FB  // CJK
         || (cp >= 0xFE10 && cp <= 0xFE6F)  // vertical, small and compatibility forms
         || (cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) ||
         (cp >= 0xFF5B && cp <= 0xFF65);
}

bool isApostrophe(const uint32_t cp) {
  return cp == '\'' || cp == 0x2019 || cp == 0x2018 || cp == 0x201B || cp == 0x02BC || cp == 0x2032 || cp == 0xFF07;
}

bool isUnspacedScript(const uint32_t unit) {
  return (utf8IsCjkBreakable(unit) && !utf8IsHangul(unit))  // Han, Kana: words are not space-delimited
         || (unit >= 0x0E00 && unit <= 0x0EFF)              // Thai, Lao
         || (unit >= 0x1000 && unit <= 0x109F)              // Myanmar
         || (unit >= 0x1780 && unit <= 0x17FF);             // Khmer
}

}  // namespace

Folded foldCodepoint(const uint32_t cp) {
  if (cp < 0x80) {
    if (cp == '\'') return ofKind(Folded::Apostrophe);
    if (isPunctuationOrSpace(cp)) return ofKind(Folded::Separator);
    return letters(asciiLower(cp));
  }
  if (isIgnorable(cp)) return ofKind(Folded::Ignore);
  if (isApostrophe(cp)) return ofKind(Folded::Apostrophe);
  if (isPunctuationOrSpace(cp)) return ofKind(Folded::Separator);

  for (const auto& multi : MULTI_LETTER_FOLDS) {
    if (multi.cp != cp) continue;
    Folded f;
    f.kind = Folded::Letters;
    for (const char* c = multi.letters; *c != '\0'; c++) f.units[f.count++] = static_cast<uint8_t>(*c);
    return f;
  }

  const int slot = latinSlot(cp);
  if (slot >= 0) {
    if (LATIN_FOLD.letters[slot] != 0) return letters(LATIN_FOLD.letters[slot]);
    // Latin-1 capitals and the Latin Extended-A/B upper/lower pairs left after decomposition.
    if (cp >= 0x00C0 && cp <= 0x00DE) return letters(cp + 0x20);
    if (cp >= 0x0100 && cp <= 0x017F && (cp & 1) == 0 && cp != 0x0138) return letters(cp + 1);
    return letters(cp);
  }

  // Greek: capitals, final sigma and the tonos/dialytika vowels.
  if (cp >= 0x0386 && cp <= 0x03CE) {
    switch (cp) {
      case 0x0386:
      case 0x03AC:
        return letters(0x03B1);
      case 0x0388:
      case 0x03AD:
        return letters(0x03B5);
      case 0x0389:
      case 0x03AE:
        return letters(0x03B7);
      case 0x038A:
      case 0x0390:
      case 0x03AA:
      case 0x03AF:
      case 0x03CA:
        return letters(0x03B9);
      case 0x038C:
      case 0x03CC:
        return letters(0x03BF);
      case 0x038E:
      case 0x03AB:
      case 0x03B0:
      case 0x03CB:
      case 0x03CD:
        return letters(0x03C5);
      case 0x038F:
      case 0x03CE:
        return letters(0x03C9);
      case 0x03C2:
        return letters(0x03C3);
      default:
        break;
    }
    if (cp >= 0x0391 && cp <= 0x03A9) return letters(cp + 0x20);
    return letters(cp);
  }

  // Cyrillic: capitals, and ё/ѐ/ѝ to е/и as readers type them.
  if (cp >= 0x0400 && cp <= 0x045F) {
    uint32_t lower = cp;
    if (cp <= 0x040F) {
      lower = cp + 0x50;
    } else if (cp <= 0x042F) {
      lower = cp + 0x20;
    }
    if (lower == 0x0450 || lower == 0x0451) return letters(0x0435);
    if (lower == 0x045D) return letters(0x0438);
    return letters(lower);
  }
  if (cp >= 0x0531 && cp <= 0x0556) return letters(cp + 0x30);  // Armenian capitals

  // Fullwidth Latin letters and digits.
  if (cp >= 0xFF10 && cp <= 0xFF19) return letters(cp - 0xFF10 + '0');
  if (cp >= 0xFF21 && cp <= 0xFF3A) return letters(cp - 0xFF21 + 'a');
  if (cp >= 0xFF41 && cp <= 0xFF5A) return letters(cp - 0xFF41 + 'a');

  return letters(cp);
}

// --- Folder ---------------------------------------------------------------------------------------------

void Folder::reset() {
  lastUnit = 0;
  pendingSeparator = false;
  pendingApostrophe = false;
}

void Folder::emit(const uint32_t unit, const uint32_t offset) {
  bool wordStart = false;
  if (unit != SEPARATOR_UNIT && unit != APOSTROPHE_UNIT) {
    wordStart = lastUnit == 0 || lastUnit == SEPARATOR_UNIT || isUnspacedScript(unit) ||
                (lastUnit != APOSTROPHE_UNIT && isUnspacedScript(lastUnit));
  }
  sink.onUnit(unit, offset, wordStart);
  lastUnit = unit;
}

void Folder::breakAt(const uint32_t offset) {
  // An apostrophe followed by a break was a closing quote, not part of a word.
  pendingApostrophe = false;
  if (lastUnit != 0 && !pendingSeparator) {
    pendingSeparator = true;
    separatorOffset = offset;
  }
}

void Folder::breakWord() { breakAt(separatorOffset); }

void Folder::feed(const uint32_t cp, const uint32_t offset) {
  const Folded folded = foldCodepoint(cp);
  switch (folded.kind) {
    case Folded::Ignore:
      return;
    case Folded::Separator:
      breakAt(offset);
      return;
    case Folded::Apostrophe:
      // Only an apostrophe that follows a letter can be inside a word; a second one in a row cannot.
      if (pendingApostrophe || pendingSeparator || lastUnit == 0 || lastUnit == SEPARATOR_UNIT) {
        breakAt(offset);
      } else {
        pendingApostrophe = true;
        apostropheOffset = offset;
      }
      return;
    case Folded::Letters:
      break;
  }

  if (pendingApostrophe) {
    pendingApostrophe = false;
    emit(APOSTROPHE_UNIT, apostropheOffset);
  } else if (pendingSeparator) {
    pendingSeparator = false;
    emit(SEPARATOR_UNIT, separatorOffset);
  }
  for (uint8_t i = 0; i < folded.count; i++) emit(folded.units[i], offset);
}

// --- PhraseMatcher --------------------------------------------------------------------------------------

namespace {

class UnitCollector final : public UnitSink {
 public:
  uint32_t* units;
  size_t capacity;
  size_t count = 0;
  bool overflow = false;

  UnitCollector(uint32_t* units, const size_t capacity) : units(units), capacity(capacity) {}

  void onUnit(const uint32_t unit, uint32_t, bool) override {
    if (count < capacity) {
      units[count++] = unit;
    } else {
      overflow = true;
    }
  }
};

}  // namespace

bool PhraseMatcher::setQuery(const std::string_view utf8Query, const bool ignoreSeparators) {
  patternLength = 0;
  reset();

  UnitCollector collector(pattern, MAX_UNITS);
  Folder folder(collector);
  const auto* cursor = reinterpret_cast<const unsigned char*>(utf8Query.data());
  const auto* end = cursor + utf8Query.size();
  while (cursor < end) {
    // The decoder reads a whole sequence; refuse a lead byte whose continuation lies past the view.
    const unsigned char lead = *cursor;
    const ptrdiff_t promised = lead < 0x80           ? 1
                               : (lead >> 5) == 0x06 ? 2
                               : (lead >> 4) == 0x0E ? 3
                               : (lead >> 3) == 0x1E ? 4
                                                     : 1;
    if (promised > end - cursor) break;
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (cp == 0) break;
    folder.feed(cp, 0);
  }
  if (collector.overflow) return false;
  if (ignoreSeparators) {
    size_t kept = 0;
    for (size_t i = 0; i < collector.count; i++) {
      if (pattern[i] != SEPARATOR_UNIT) pattern[kept++] = pattern[i];
    }
    collector.count = kept;
  }
  if (collector.count == 0) return false;

  patternLength = static_cast<uint8_t>(collector.count);
  failure[0] = 0;
  uint8_t k = 0;
  for (uint8_t i = 1; i < patternLength; i++) {
    while (k > 0 && pattern[i] != pattern[k]) k = failure[k - 1];
    if (pattern[i] == pattern[k]) k++;
    failure[i] = k;
  }
  return true;
}

void PhraseMatcher::reset() {
  matchedLength = 0;
  unitCount = 0;
  recentWordStarts = 0;
}

void PhraseMatcher::onUnit(const uint32_t unit, const uint32_t offset, const bool wordStart) {
  if (patternLength == 0) return;
  const size_t slot = unitCount % MAX_UNITS;
  recentOffsets[slot] = offset;
  const uint64_t bit = uint64_t{1} << slot;
  recentWordStarts = wordStart ? (recentWordStarts | bit) : (recentWordStarts & ~bit);
  unitCount++;

  while (matchedLength > 0 && pattern[matchedLength] != unit) matchedLength = failure[matchedLength - 1];
  if (pattern[matchedLength] == unit) matchedLength++;
  if (matchedLength < patternLength) return;

  const size_t startSlot = (unitCount - patternLength) % MAX_UNITS;
  if (recentWordStarts & (uint64_t{1} << startSlot)) sink.onMatch(recentOffsets[startSlot], offset + 1);
  matchedLength = failure[patternLength - 1];
}

// --- PageMatchLocator -----------------------------------------------------------------------------------

void PageMatchLocator::addWord(const std::string_view utf8Word, const uint16_t index) {
  const auto* cursor = reinterpret_cast<const unsigned char*>(utf8Word.data());
  const auto* end = cursor + utf8Word.size();
  while (cursor < end) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (cp == 0) break;
    folder.feed(cp, index);
  }
  folder.breakWord();
}

void PageMatchLocator::onUnit(const uint32_t unit, const uint32_t offset, const bool wordStart) {
  if (unit == SEPARATOR_UNIT) return;
  units++;
  matcher.onUnit(unit, offset, wordStart);
}

void PageMatchLocator::onMatch(const uint32_t start, const uint32_t end) {
  if (occurrenceCount == MAX_OCCURRENCES) return;
  occurrences[occurrenceCount++] = {static_cast<uint16_t>(start), static_cast<uint16_t>(end - 1), units};
}

bool PageMatchLocator::nearest(const float position, uint16_t& firstWord, uint16_t& lastWord) const {
  if (occurrenceCount == 0) return false;
  const float target = position * static_cast<float>(units);
  size_t best = 0;
  float bestDistance = -1.0f;
  for (size_t i = 0; i < occurrenceCount; i++) {
    const float at = static_cast<float>(occurrences[i].unit);
    const float distance = at > target ? at - target : target - at;
    if (bestDistance < 0.0f || distance < bestDistance) {
      best = i;
      bestDistance = distance;
    }
  }
  firstWord = occurrences[best].firstWord;
  lastWord = occurrences[best].lastWord;
  return true;
}

// --- ExcerptBuilder -------------------------------------------------------------------------------------

namespace {

bool isDisplaySpace(const uint32_t cp) {
  return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200A) ||
         cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

constexpr char ELLIPSIS[] = "\xE2\x80\xA6";
constexpr size_t ELLIPSIS_BYTES = sizeof(ELLIPSIS) - 1;
// Past AFTER_CHARS, an excerpt runs on to the end of the word, but no further than this.
constexpr size_t WORD_END_SLACK = 16;

}  // namespace

void ExcerptBuilder::reset() {
  breakBlock();
  state = State::Idle;
  textLength = 0;
  text[0] = '\0';
}

void ExcerptBuilder::breakBlock() {
  ringStart = 0;
  ringSize = 0;
  ringDropped = false;
}

bool ExcerptBuilder::appendBytes(const char* bytes, const size_t len) {
  if (textLength + len >= MAX_BYTES) return false;
  memcpy(text + textLength, bytes, len);
  textLength += len;
  text[textLength] = '\0';
  return true;
}

void ExcerptBuilder::append(const uint32_t cp) {
  if (cp == ' ') {
    if (lastWasSpace) return;
    lastWasSpace = true;
  } else {
    lastWasSpace = false;
  }
  char bytes[4];
  size_t len = 0;
  if (cp < 0x80) {
    bytes[len++] = static_cast<char>(cp);
  } else if (cp < 0x800) {
    bytes[len++] = static_cast<char>(0xC0 | (cp >> 6));
    bytes[len++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    bytes[len++] = static_cast<char>(0xE0 | (cp >> 12));
    bytes[len++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    bytes[len++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    bytes[len++] = static_cast<char>(0xF0 | (cp >> 18));
    bytes[len++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    bytes[len++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    bytes[len++] = static_cast<char>(0x80 | (cp & 0x3F));
  }
  // Keep room to close with an ellipsis.
  if (textLength + len + ELLIPSIS_BYTES >= MAX_BYTES) {
    appendBytes(ELLIPSIS, ELLIPSIS_BYTES);
    state = State::Complete;
    return;
  }
  appendBytes(bytes, len);
}

void ExcerptBuilder::push(uint32_t cp, const uint32_t offset) {
  if (isDisplaySpace(cp)) {
    cp = ' ';
  } else if (!utf8IsCombiningMark(cp) && foldCodepoint(cp).kind == Folded::Ignore) {
    return;
  }

  const bool collapse = cp == ' ' && ringSize > 0 && ringCodepoints[(ringStart + ringSize - 1) % RING] == ' ';
  if (!collapse) {
    if (ringSize == RING) {
      ringStart = (ringStart + 1) % RING;
      ringSize--;
      ringDropped = true;
    }
    const size_t slot = (ringStart + ringSize) % RING;
    ringCodepoints[slot] = cp;
    ringOffsets[slot] = offset;
    ringSize++;
  }

  if (state == State::Collecting || state == State::Trailing) {
    if (state == State::Trailing && (cp == ' ' || charsAfter >= AFTER_CHARS + WORD_END_SLACK)) {
      while (textLength > 0 && text[textLength - 1] == ' ') text[--textLength] = '\0';
      appendBytes(ELLIPSIS, ELLIPSIS_BYTES);
      state = State::Complete;
      return;
    }
    append(cp);
    if (state == State::Complete) return;
    if (offset >= matchEnd && cp != ' ') charsAfter++;
    if (state == State::Collecting && charsAfter >= AFTER_CHARS) state = State::Trailing;
  }
}

void ExcerptBuilder::beginMatch(const uint32_t start, const uint32_t end) {
  state = State::Collecting;
  matchEnd = end;
  charsAfter = 0;
  textLength = 0;
  text[0] = '\0';
  lastWasSpace = true;

  // The match's first codepoint, or the ring's start when a very long match outran it.
  size_t first = 0;
  while (first < ringSize && ringOffsets[(ringStart + first) % RING] < start) first++;
  if (first == ringSize) first = 0;

  size_t from = first > BEFORE_CHARS ? first - BEFORE_CHARS : 0;
  const bool cut = from > 0 || ringDropped;
  if (cut) {
    // Start on a word: skip the partial word the cut landed in.
    size_t space = from;
    while (space < first && ringCodepoints[(ringStart + space) % RING] != ' ') space++;
    if (space < first) from = space;
    appendBytes(ELLIPSIS, ELLIPSIS_BYTES);
  }
  for (size_t i = from; i < ringSize && state != State::Complete; i++) {
    const size_t slot = (ringStart + i) % RING;
    append(ringCodepoints[slot]);
    if (ringOffsets[slot] >= end && ringCodepoints[slot] != ' ') charsAfter++;
  }
}

const char* ExcerptBuilder::finish(const bool more) {
  if (state == State::Collecting || state == State::Trailing) {
    while (textLength > 0 && text[textLength - 1] == ' ') text[--textLength] = '\0';
    if (more) appendBytes(ELLIPSIS, ELLIPSIS_BYTES);
  }
  state = State::Idle;
  utf8ComposeNfcInPlace(text);
  return text;
}

}  // namespace booksearch

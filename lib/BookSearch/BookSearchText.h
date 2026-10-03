#pragma once

// Text model for searching inside a book: the fold that makes "Nirvāṇa", "NIRVANA" and "nirvana" the same
// word, a streaming phrase matcher over folded text, and the excerpt shown for each match.
//
// Pure C++ over codepoints that carry the caller's offsets: no hardware, no allocation. Host-tested in
// test/book_search.
//
// The fold drops case and accents (precomposed or combining), maps the letters that have no decomposition
// (ø, ł, ß, æ, ...), ligatures and fullwidth forms, and turns whitespace and punctuation into word breaks.
// A run of breaks counts as one, so a remembered phrase is found whatever punctuation the book put between
// its words: "to be or not" finds "To be, or not". An apostrophe between two letters stays part of the word,
// in either form, so "don't" finds "don’t" while a ‘quoted’ word still starts a word.
//
// A match must begin at the start of a word and may end anywhere: "vajra" finds "Vajrasattva" and
// "vajra-holder" but not "ovajra". Scripts written without spaces (Han, Kana, Thai, ...) start a word at
// every character.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace booksearch {

constexpr uint32_t SEPARATOR_UNIT = ' ';
constexpr uint32_t APOSTROPHE_UNIT = '\'';

// What one source codepoint contributes to the folded stream.
struct Folded {
  enum Kind : uint8_t { Ignore, Separator, Apostrophe, Letters };
  Kind kind = Ignore;
  uint8_t count = 0;       // letters written to `units` (Letters only)
  uint32_t units[3] = {};  // lowercase, accent-free letters or digits
};

Folded foldCodepoint(uint32_t cp);

// Receives folded units. `offset` is the caller's offset of the source codepoint the unit came from.
class UnitSink {
 public:
  virtual void onUnit(uint32_t unit, uint32_t offset, bool wordStart) = 0;

 protected:
  ~UnitSink() = default;
};

// Turns source codepoints into folded units: a run of breaks becomes one SEPARATOR_UNIT, breaks at either
// end are dropped, and an apostrophe survives only between two letters.
class Folder {
 public:
  explicit Folder(UnitSink& sink) : sink(sink) {}

  void feed(uint32_t cp, uint32_t offset);
  // A block edge (paragraph, heading, line break): separates words without being text.
  void breakWord();
  void reset();

 private:
  void emit(uint32_t unit, uint32_t offset);
  void breakAt(uint32_t offset);

  UnitSink& sink;
  uint32_t lastUnit = 0;  // 0: nothing emitted since reset()
  bool pendingSeparator = false;
  uint32_t separatorOffset = 0;
  bool pendingApostrophe = false;
  uint32_t apostropheOffset = 0;
};

// A completed match: [start, end) in the caller's offsets of the source codepoints it covers.
class MatchSink {
 public:
  virtual void onMatch(uint32_t start, uint32_t end) = 0;

 protected:
  ~MatchSink() = default;
};

// Finds a folded phrase in a folded stream (Knuth-Morris-Pratt), reporting only matches that begin a word.
class PhraseMatcher final : public UnitSink {
 public:
  // Longest query, in folded units. Keyboard input is capped below this (see QUERY_MAX_BYTES).
  static constexpr size_t MAX_UNITS = 64;

  explicit PhraseMatcher(MatchSink& sink) : sink(sink) {}

  // Folds the query. False when nothing searchable is left: empty, only punctuation, or too long.
  // `ignoreSeparators` matches letters only, for text whose word breaks are not the source's (a laid-out
  // page); the stream must then leave out SEPARATOR_UNIT too.
  bool setQuery(std::string_view utf8Query, bool ignoreSeparators = false);
  size_t length() const { return patternLength; }
  // Forget partial progress; call between documents.
  void reset();

  void onUnit(uint32_t unit, uint32_t offset, bool wordStart) override;

 private:
  MatchSink& sink;
  uint32_t pattern[MAX_UNITS] = {};
  uint8_t failure[MAX_UNITS] = {};
  uint8_t patternLength = 0;
  uint8_t matchedLength = 0;
  // The source offset and word-start flag of the last MAX_UNITS units, to find where a match began.
  uint32_t recentOffsets[MAX_UNITS] = {};
  uint64_t recentWordStarts = 0;
  uint32_t unitCount = 0;
};

// Longest query accepted from the keyboard, in bytes.
constexpr size_t QUERY_MAX_BYTES = 120;

// Finds a search result among the words laid out on a page, to mark it there. Layout drops the spaces,
// splits words at line ends and style changes, and adds hyphens, so only letters are compared; a match
// still has to begin a word.
class PageMatchLocator final : private UnitSink, private MatchSink {
 public:
  PageMatchLocator() = default;
  PageMatchLocator(const PageMatchLocator&) = delete;
  PageMatchLocator& operator=(const PageMatchLocator&) = delete;

  bool setQuery(std::string_view utf8Query) { return matcher.setQuery(utf8Query, /*ignoreSeparators=*/true); }
  // The page's words in reading order; `index` is the caller's id for the word.
  void addWord(std::string_view utf8Word, uint16_t index);
  // The occurrence nearest `position` (0 = the page's first letter, 1 = its last), as the first and last
  // word it covers. False when the page does not contain the query.
  bool nearest(float position, uint16_t& firstWord, uint16_t& lastWord) const;

 private:
  static constexpr size_t MAX_OCCURRENCES = 16;
  struct Occurrence {
    uint16_t firstWord;
    uint16_t lastWord;
    uint32_t unit;
  };

  void onUnit(uint32_t unit, uint32_t offset, bool wordStart) override;
  void onMatch(uint32_t start, uint32_t end) override;

  PhraseMatcher matcher{*this};
  Folder folder{*this};
  uint32_t units = 0;
  Occurrence occurrences[MAX_OCCURRENCES] = {};
  size_t occurrenceCount = 0;
};

// Keeps the recent text of the current block so a match can be shown in context, then collects what follows
// it. Excerpts never cross a block edge, so the context is the paragraph the match is in.
class ExcerptBuilder {
 public:
  static constexpr size_t MAX_BYTES = 192;  // including the terminating NUL
  static constexpr size_t BEFORE_CHARS = 40;
  static constexpr size_t AFTER_CHARS = 72;

  // Display text in reading order. Whitespace runs collapse to one space; invisible characters are dropped.
  void push(uint32_t cp, uint32_t offset);
  void breakBlock();
  void reset();

  // Starts the excerpt for a match beginning at `start`. A pending excerpt must be finished first.
  void beginMatch(uint32_t start, uint32_t end);
  bool pending() const { return state != State::Idle; }
  // True once enough text after the match has arrived.
  bool complete() const { return state == State::Complete; }
  // Ends the pending excerpt with what it has and returns it (NUL-terminated UTF-8, NFC). `more` marks text
  // left unshown after it. Valid until the next beginMatch().
  const char* finish(bool more);

 private:
  static constexpr size_t RING = 160;
  enum class State : uint8_t { Idle, Collecting, Trailing, Complete };

  void append(uint32_t cp);
  bool appendBytes(const char* bytes, size_t len);

  // Text since the block began, newest last.
  uint32_t ringCodepoints[RING] = {};
  uint32_t ringOffsets[RING] = {};
  size_t ringStart = 0;
  size_t ringSize = 0;
  // Earlier text of this block fell out of the ring.
  bool ringDropped = false;

  State state = State::Idle;
  uint32_t matchEnd = 0;
  size_t charsAfter = 0;
  bool lastWasSpace = true;
  char text[MAX_BYTES] = {};
  size_t textLength = 0;
};

}  // namespace booksearch

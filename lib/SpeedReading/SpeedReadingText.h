#pragma once

// The speed reader's text model: the words it shows, how long each one stays, where the eye fixes on it, and
// how words join into one flash when the panel cannot change as fast as the reading pace asks.

#include <cstddef>
#include <cstdint>

namespace speedread {

enum WordFlags : uint8_t {
  PARAGRAPH_END = 1 << 0,  // last word of a block: a paragraph, heading, list item, verse line
  CHAPTER_END = 1 << 1,    // last word of a spine item
  CONTINUES = 1 << 2,      // a piece of a word too long for one entry; the next entry carries on with it
};

struct Word {
  // UTF-8, NUL-terminated. A longer word arrives in pieces (CONTINUES).
  static constexpr size_t TEXT_BYTES = 32;
  char text[TEXT_BYTES];
  // Visible-text offset of the first codepoint, counted as ChapterHtmlSlimParser counts page starts.
  uint32_t offset;
  uint16_t spine;
  // Position in the spine's XHTML, in thousandths, for the book position readout.
  uint16_t permille;
  uint8_t length;  // bytes in text
  uint8_t flags;
};

enum class Pause : uint8_t { None, Clause, Sentence };

// The pause the word's own punctuation asks for, looking through closing quotes and brackets: a clause pause
// after , ; : and dashes, a sentence pause after . ! ? and the ellipsis. Common abbreviations (Mr. e.g.) take
// none.
Pause trailingPause(const char* text, size_t length);

// True when the text has a letter, digit or symbol, not only punctuation.
bool hasContent(const char* text, size_t length);

// The letter the eye should fix on (the optimal recognition point): a little left of the middle, counted
// over the word without its surrounding punctuation. Byte range within `text`.
struct Pivot {
  uint8_t start;
  uint8_t length;
};
Pivot pivotOf(const char* text, size_t length);

// How long the word stays at `wpm`: longer words, punctuation and the end of a paragraph or chapter hold
// the eye a little longer, as they do in ordinary reading.
uint32_t wordDwellMs(const Word& word, uint16_t wpm);

struct ChunkRules {
  uint16_t wpm;
  // The panel's refresh time. A flash cannot show for less, so words join until their time covers it.
  uint32_t minFlashMs;
  uint8_t maxWords;
};

// The number of words in the next flash, from the first `count` upcoming words (count >= 1 gives >= 1).
// A flash never runs past punctuation, the end of a paragraph or chapter, or a word piece, and holds only
// words for which `fits(context, n)` says the first n words fit on the line. `dwellMs` gets the flash's time.
size_t planChunk(const Word* const* words, size_t count, const ChunkRules& rules, bool (*fits)(void*, size_t),
                 void* context, uint32_t& dwellMs);

}  // namespace speedread

#include "SpeedReadingText.h"

#include <Utf8.h>
#include <strings.h>

#include <algorithm>
#include <cstring>

namespace speedread {

namespace {

constexpr size_t MAX_CODEPOINTS = Word::TEXT_BYTES;

bool isPunctuation(const uint32_t cp) {
  if (cp < 0x80) {
    return std::strchr("!\"'()*,-./:;?[]_{}", static_cast<int>(cp)) != nullptr && cp != 0;
  }
  return cp == 0x00A1 || cp == 0x00AB || cp == 0x00B7 || cp == 0x00BB || cp == 0x00BF || cp == 0x060C || cp == 0x061B ||
         cp == 0x061F || cp == 0x06D4 || cp == 0x0964 || cp == 0x0965 || (cp >= 0x2010 && cp <= 0x2027) ||
         cp == 0x2039 || cp == 0x203A || cp == 0x203C || (cp >= 0x2047 && cp <= 0x2049) ||
         (cp >= 0x3001 && cp <= 0x3003) || (cp >= 0x3008 && cp <= 0x3011) || (cp >= 0x3014 && cp <= 0x301F) ||
         cp == 0xFF01 || cp == 0xFF08 || cp == 0xFF09 || cp == 0xFF0C || cp == 0xFF0E || cp == 0xFF1A || cp == 0xFF1B ||
         cp == 0xFF1F;
}

// Marks that close a phrase without ending it: the pause belongs to the punctuation inside them.
bool isClosingMark(const uint32_t cp) {
  return cp == '"' || cp == '\'' || cp == ')' || cp == ']' || cp == '}' || cp == '*' || cp == 0x00BB || cp == 0x2019 ||
         cp == 0x201D || cp == 0x203A || cp == 0x300D || cp == 0x300F || cp == 0xFF09;
}

bool endsSentence(const uint32_t cp) {
  return cp == '.' || cp == '!' || cp == '?' || cp == 0x2026 || cp == 0x203C || (cp >= 0x2047 && cp <= 0x2049) ||
         cp == 0x3002 || cp == 0xFF01 || cp == 0xFF0E || cp == 0xFF1F || cp == 0x061F || cp == 0x06D4 || cp == 0x0964 ||
         cp == 0x0965;
}

bool endsClause(const uint32_t cp) {
  return cp == ',' || cp == ';' || cp == ':' || (cp >= 0x2012 && cp <= 0x2015) || cp == 0x060C || cp == 0x061B ||
         cp == 0x3001 || cp == 0xFF0C || cp == 0xFF1A || cp == 0xFF1B;
}

// Abbreviations whose full stop does not end a sentence. Compared without their final stop.
constexpr const char* ABBREVIATIONS[] = {"mr", "mrs", "ms", "dr", "st", "jr", "sr", "vs", "e.g", "i.e", "cf", "fig"};

struct Codepoints {
  uint32_t cp[MAX_CODEPOINTS];
  uint8_t byte[MAX_CODEPOINTS + 1];  // byte offset of each codepoint, plus the end
  size_t count = 0;
};

void decode(const char* text, const size_t length, Codepoints& out) {
  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  const auto* limit = cursor + std::min(length, MAX_CODEPOINTS - 1);
  out.count = 0;
  while (cursor < limit && *cursor && out.count < MAX_CODEPOINTS) {
    out.byte[out.count] = static_cast<uint8_t>(cursor - reinterpret_cast<const unsigned char*>(text));
    out.cp[out.count++] = utf8NextCodepoint(&cursor);
  }
  out.byte[out.count] = static_cast<uint8_t>(cursor - reinterpret_cast<const unsigned char*>(text));
}

// Letters, digits and symbols: what the word's length is counted in.
size_t contentLength(const Codepoints& word) {
  size_t letters = 0;
  for (size_t i = 0; i < word.count; i++) {
    if (!isPunctuation(word.cp[i]) && !utf8IsCombiningMark(word.cp[i]) && word.cp[i] != ' ') letters++;
  }
  return letters;
}

bool isAbbreviation(const char* text, const size_t length) {
  if (length < 2 || text[length - 1] != '.') return false;
  // An initial: one capital letter and its stop ("J.").
  if (length == 2 && text[0] >= 'A' && text[0] <= 'Z') return true;
  const size_t core = length - 1;
  for (const char* abbreviation : ABBREVIATIONS) {
    if (strlen(abbreviation) == core && strncasecmp(text, abbreviation, core) == 0) return true;
  }
  return false;
}

}  // namespace

Pause trailingPause(const char* text, const size_t length) {
  Codepoints word;
  decode(text, length, word);
  size_t i = word.count;
  while (i > 0 && isClosingMark(word.cp[i - 1])) i--;
  if (i == 0) return Pause::None;
  const uint32_t last = word.cp[i - 1];
  if (endsSentence(last)) {
    return last == '.' && i == word.count && isAbbreviation(text, length) ? Pause::None : Pause::Sentence;
  }
  return endsClause(last) ? Pause::Clause : Pause::None;
}

bool hasContent(const char* text, const size_t length) {
  Codepoints word;
  decode(text, length, word);
  return contentLength(word) > 0;
}

Pivot pivotOf(const char* text, const size_t length) {
  Codepoints word;
  decode(text, length, word);
  if (word.count == 0) return {0, 0};

  size_t first = 0;
  while (first < word.count && isPunctuation(word.cp[first])) first++;
  size_t last = word.count;
  while (last > first && isPunctuation(word.cp[last - 1])) last--;

  size_t index;
  if (first >= last) {
    index = word.count / 2;
  } else {
    // Spritz's table: the recognition point moves right one letter per four letters of length, up to the fifth.
    const size_t letters = contentLength(word);
    const size_t point = letters <= 1 ? 0 : letters <= 5 ? 1 : letters <= 9 ? 2 : letters <= 13 ? 3 : 4;
    // Step over `point` letters, not combining marks, from the first one.
    index = first;
    for (size_t seen = 0; index < last; index++) {
      if (utf8IsCombiningMark(word.cp[index])) continue;
      if (seen++ == point) break;
    }
    if (index >= last) index = last - 1;
  }
  size_t end = index + 1;
  while (end < word.count && utf8IsCombiningMark(word.cp[end])) end++;
  return {word.byte[index], static_cast<uint8_t>(word.byte[end] - word.byte[index])};
}

uint32_t wordDwellMs(const Word& word, const uint16_t wpm) {
  const uint32_t base = 60000u / std::max<uint16_t>(wpm, 1);
  Codepoints decoded;
  decode(word.text, word.length, decoded);
  const size_t letters = contentLength(decoded);
  uint32_t permille = 1000;
  if (letters > 6) permille += static_cast<uint32_t>(std::min<size_t>(600, 50 * (letters - 6)));
  if (word.flags & CHAPTER_END) {
    permille *= 3;
  } else if (word.flags & PARAGRAPH_END) {
    permille = permille * 5 / 2;
  } else {
    switch (trailingPause(word.text, word.length)) {
      case Pause::Sentence:
        permille *= 2;
        break;
      case Pause::Clause:
        permille = permille * 3 / 2;
        break;
      case Pause::None:
        break;
    }
  }
  return base * permille / 1000;
}

size_t planChunk(const Word* const* words, const size_t count, const ChunkRules& rules, bool (*fits)(void*, size_t),
                 void* context, uint32_t& dwellMs) {
  dwellMs = 0;
  const size_t limit = std::min<size_t>(count, std::max<uint8_t>(rules.maxWords, 1));
  size_t n = 0;
  while (n < limit) {
    if (n > 0 && fits && !fits(context, n + 1)) break;
    const Word& word = *words[n];
    dwellMs += wordDwellMs(word, rules.wpm);
    n++;
    if (word.flags & (PARAGRAPH_END | CHAPTER_END | CONTINUES)) break;
    if (trailingPause(word.text, word.length) != Pause::None) break;
    if (dwellMs >= rules.minFlashMs) break;
  }
  return n;
}

}  // namespace speedread

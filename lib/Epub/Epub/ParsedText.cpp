#include "ParsedText.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <string_view>
#include <vector>

#include "TokenBoundary.h"
#include "hyphenation/HyphenationCommon.h"
#include "hyphenation/Hyphenator.h"

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
// Paragraph-level direction: scan the first N words to find base direction.
constexpr size_t RTL_PARAGRAPH_PROBE_WORDS = 3;
// Per-word: scan enough chars to see through leading neutrals (quotes, numbers)
// before giving up. 64 is a hedge for pathological cases like long numeric tokens.
constexpr int RTL_PER_WORD_PROBE_DEPTH = 64;

// Byte-level pre-check: Hebrew UTF-8 lead bytes 0xD6-0xD7, Arabic/Syriac 0xD8-0xDB.
bool mayContainRtlBytes(const char* str) {
  for (const auto* p = reinterpret_cast<const unsigned char*>(str); *p; ++p) {
    if (*p >= 0xD6 && *p <= 0xDB) return true;
  }
  return false;
}

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string_view word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.data());
  const auto* const end = ptr + word.size();
  while (ptr < end) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
  return 0;
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string_view word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.data() + i);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(const std::string_view word) { return word.find(SOFT_HYPHEN_UTF8) != std::string_view::npos; }

bool isNoBreakBeforeCjkPunctuation(const uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case ':':
    case ';':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
    case 0x00BB:  // »
    case 0x2019:  // ’
    case 0x201D:  // ”
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3017:  // 〗
    case 0x3019:  // 〙
    case 0x301B:  // 〛
    case 0xFF01:  // ！
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
      return true;
    default:
      return false;
  }
}

bool isNoBreakAfterCjkPunctuation(const uint32_t cp) {
  switch (cp) {
    case '(':
    case '[':
    case '{':
    case 0x00AB:  // «
    case 0x2018:  // ‘
    case 0x201C:  // “
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3016:  // 〖
    case 0x3018:  // 〘
    case 0x301A:  // 〚
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
      return true;
    default:
      return false;
  }
}

bool containsCjkBreakableCodepoint(const std::string& text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (utf8IsCjkBreakable(cp)) {
      return true;
    }
  }
  return false;
}

uint32_t countCodepoints(const std::string_view text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.data());
  const auto* const end = ptr + text.size();
  uint32_t count = 0;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }
  return count;
}

bool cjkBoundaryAllowsBreak(const uint32_t leftCp, const uint32_t rightCp) {
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoBreakAfterCjkPunctuation(leftCp) || isNoBreakBeforeCjkPunctuation(rightCp)) return false;
  if (utf8IsCombiningMark(rightCp)) return false;
  return true;
}

// Korean separates words with spaces, so a boundary touching Hangul is not a gap-less break inside
// a line. hangulLineEndBreaks() still lets a Hangul word split there at a line end.
bool hasCjkBreakOpportunityBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (utf8IsHangul(leftCp) || utf8IsHangul(rightCp)) return false;
  return cjkBoundaryAllowsBreak(leftCp, rightCp);
}

// Line-end split points inside a Hangul word, using the CJK boundary rules (no hyphen is drawn).
std::vector<Hyphenator::BreakInfo> hangulLineEndBreaks(const std::string& word) {
  std::vector<Hyphenator::BreakInfo> breaks;
  if (word.empty()) return breaks;
  const auto* const start = reinterpret_cast<const unsigned char*>(word.c_str());
  const auto* ptr = start;
  uint32_t prev = utf8NextCodepoint(&ptr);
  while (*ptr) {
    const size_t offset = static_cast<size_t>(ptr - start);
    const uint32_t cur = utf8NextCodepoint(&ptr);
    if ((utf8IsHangul(prev) || utf8IsHangul(cur)) && cjkBoundaryAllowsBreak(prev, cur)) {
      breaks.push_back({offset, false});
    }
    prev = cur;
  }
  return breaks;
}

std::vector<size_t> cjkCharacterBreakByteOffsets(const std::string& text) {
  struct CodepointBoundary {
    uint32_t cp;
    size_t endOffset;
  };

  std::vector<CodepointBoundary> codepoints;
  codepoints.reserve(text.size());
  bool hasCjkBreakable = false;

  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const start = ptr;
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) break;
    if (utf8IsCjkBreakable(cp)) {
      hasCjkBreakable = true;
    }
    codepoints.push_back({cp, static_cast<size_t>(ptr - start)});
  }

  if (!hasCjkBreakable || codepoints.size() < 2) return {};

  std::vector<size_t> allowedOffsets;
  allowedOffsets.reserve(codepoints.size() - 1);
  for (size_t i = 0; i + 1 < codepoints.size(); ++i) {
    const uint32_t current = codepoints[i].cp;
    const uint32_t next = codepoints[i + 1].cp;
    if (!hasCjkBreakOpportunityBetween(current, next)) continue;
    allowedOffsets.push_back(codepoints[i].endOffset);
  }
  return allowedOffsets;
}

// Hands out `spare` pixels (negative to tighten) over `slots` gaps, one gap per next() call. Shares differ by at
// most one pixel and sum exactly to `spare`, so a justified line ends flush on the margin, and the odd pixels are
// spread along the line instead of bunching at one end. A large per-gap share is legitimate: a sparse line still
// has to reach the margin.
class GapDistributor {
 public:
  GapDistributor(const int spare, const int slots) : spare(spare), slots(slots) {}

  int next() {
    if (slots <= 0) return 0;
    const int before = shareThrough(index);
    ++index;
    return shareThrough(index) - before;
  }

 private:
  int shareThrough(const int gaps) const { return spare >= 0 ? spare * gaps / slots : -(-spare * gaps / slots); }

  int spare;
  int slots;
  int index = 0;
};

// Justifies one line. Spare width goes to every justifiable gap; a shortfall (a line the breaker set tight) comes
// out of real spaces only -- CJK break opportunities have no width to give -- and never beyond maxShrink.
class LineJustifier {
 public:
  LineJustifier(const bool justify, const int spare, const int stretchGaps, const int shrinkGaps, const int maxShrink)
      : stretching(justify && spare > 0 && stretchGaps > 0),
        shrinking(justify && spare < 0 && shrinkGaps > 0),
        amount(stretching ? spare : (shrinking ? std::max(spare, -maxShrink) : 0)),
        distributor(amount, stretching ? stretchGaps : shrinkGaps) {}

  // Extra advance for the next gap, in placement order.
  int nextGap(const bool stretches, const bool shrinks) {
    return (stretching ? stretches : (shrinking && shrinks)) ? distributor.next() : 0;
  }
  // Total width the line's gaps gain (negative: lose).
  int applied() const { return amount; }

 private:
  bool stretching;
  bool shrinking;
  int amount;
  GapDistributor distributor;
};

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

constexpr int scaleSpace(const int advance, const uint8_t wordSpacingPercent) {
  return wordSpacingPercent == 100 ? advance : (advance * wordSpacingPercent + 50) / 100;
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
// `word` is usually NUL-terminated at data()[size()] (arena entries and
// std::string arguments), but focus-split candidates pass PREFIX views of a
// stored word. Probing data()[size()] is in-bounds for every caller (it is
// either the word's own NUL or a mid-word byte of the same arena entry) and
// routes unterminated views through the copying path so the C API never
// measures past the view.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string_view word,
                          const EpdFontFamily::Style style, const int8_t characterSpacing,
                          const uint8_t wordSpacingPercent, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return scaleSpace(renderer.getSpaceWidth(fontId, style), wordSpacingPercent);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen && word.data()[word.size()] == '\0') {
    return renderer.getTextAdvanceX(fontId, word.data(), style, characterSpacing);
  }

  std::string sanitized(word);
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style, characterSpacing);
}

// True when a token ends with a hyphen or dash that allows a line break after it. U+00AD is
// excluded because it renders as nothing, so only a hyphenation break -- which substitutes a
// visible '-' -- may use it; U+2011 because forbidding that break is its purpose.
bool endsWithBreakableHyphen(const std::string_view token) {
  if (token.empty()) return false;
  return TokenBoundary::allowsBreakAfterExplicitHyphen(lastCodepoint(token));
}

// Focus Reading renders the first `focusBoundary` bytes of a token bold and the rest at the
// token's own style; 0 means no emphasis. The bold run is at most 9 codepoints (see addWord),
// so it is copied to a stack buffer rather than a substring: this runs once per word during
// pagination and must not allocate.
constexpr size_t FOCUS_PREFIX_BUF_SIZE = 40;

// Advance from the token origin to the start of its regular-weight suffix: the bold prefix plus
// the kerning across the weight change. Doubles as the suffix's x offset inside the word.
uint16_t measureFocusPrefixAdvance(const GfxRenderer& renderer, const int fontId, const std::string_view word,
                                   const EpdFontFamily::Style style, const uint8_t focusBoundary,
                                   const int8_t characterSpacing) {
  char prefixBuf[FOCUS_PREFIX_BUF_SIZE];
  const size_t prefixLen = std::min<size_t>(focusBoundary, FOCUS_PREFIX_BUF_SIZE - 1);
  memcpy(prefixBuf, word.data(), prefixLen);
  prefixBuf[prefixLen] = '\0';

  const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  const auto* suffixPtr = reinterpret_cast<const unsigned char*>(word.data() + focusBoundary);
  const int kerning =
      renderer.getKerning(fontId, lastCodepoint(prefixBuf), utf8NextCodepoint(&suffixPtr), boldStyle, characterSpacing);
  return static_cast<uint16_t>(renderer.getTextAdvanceX(fontId, prefixBuf, boldStyle, characterSpacing) + kerning);
}

// Advance width of a whole token, accounting for a bold focus prefix when it has one.
uint16_t measureFocusWordWidth(const GfxRenderer& renderer, const int fontId, const std::string_view word,
                               const EpdFontFamily::Style style, const uint8_t focusBoundary,
                               const int8_t characterSpacing, const uint8_t wordSpacingPercent,
                               const bool appendHyphen = false) {
  if (focusBoundary == 0) {
    return measureWordWidth(renderer, fontId, word, style, characterSpacing, wordSpacingPercent, appendHyphen);
  }
  if (focusBoundary >= word.size()) {
    // The bold run covers the whole token, as for a split candidate ending on the boundary.
    return measureWordWidth(renderer, fontId, word, static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD),
                            characterSpacing, wordSpacingPercent, appendHyphen);
  }
  // Through measureWordWidth (not getTextAdvanceX directly): `word` itself can
  // be an unterminated prefix view here, and the suffix view inherits that.
  const uint16_t suffixWidth = measureWordWidth(renderer, fontId, word.substr(focusBoundary), style, characterSpacing,
                                                wordSpacingPercent, appendHyphen);
  return measureFocusPrefixAdvance(renderer, fontId, word, style, focusBoundary, characterSpacing) + suffixWidth;
}

// Focus boundaries for the two halves of a token split at `splitOffset`.
uint8_t focusBoundaryBefore(const uint8_t focusBoundary, const size_t splitOffset) {
  return static_cast<uint8_t>(std::min<size_t>(focusBoundary, splitOffset));
}
uint8_t focusBoundaryAfter(const uint8_t focusBoundary, const size_t splitOffset) {
  return focusBoundary > splitOffset ? static_cast<uint8_t>(focusBoundary - splitOffset) : 0;
}

// Checks if a UTF-8 codepoint should be counted as part of a word for Focus Reading
bool isWordCharacter(uint32_t cp) {
  // ASCII range (Catches 95%+ of characters immediately)
  if (cp < 128) {
    // Bitwise trick: (cp | 0x20) converts uppercase ASCII to lowercase.
    // This checks for A-Z and a-z mathematically, avoiding memory lookups and <cctype>
    return ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || cp == '\'';
  }

  // General Punctuation Block, Currency, Math, Arrows, & Symbols (0x2000 - 0x2BFF)
  if (cp >= 0x2000 && cp <= 0x2BFF) {
    // Explicitly allow smart quotes, reject all other general punctuation (em-dashes, etc.)
    return cp == 0x2018 || cp == 0x2019;
  }

  // Latin-1 Punctuation Block (0x00A1 - 0x00BF)
  if (cp >= 0x00A1 && cp <= 0x00BF) {
    // Allow ordinal indicators and micro sign, reject the rest (¡, ¿, «, », etc.)
    return cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA;
  }

  // Rejects Two-em dash, Three-em dash, Double oblique hyphen, etc.
  if (cp >= 0x2E00 && cp <= 0x2E7F) return false;

  // Rejects Modifier Minus (0x02D7), Small Hyphen (0xFE63), and Fullwidth Hyphen (0xFF0D)
  if (cp == 0x02D7 || cp == 0xFE63 || cp == 0xFF0D) return false;
  // Assume all other Unicode ranges (accented letters, Cyrillic, Greek, etc.) are valid

  return true;
}

// --- Knuth-Plass line breaking ---------------------------------------------------------------------------------
// Total-fit breaking as in TeX (Knuth & Plass, "Breaking Paragraphs into Lines", 1981), with TeX's default
// parameters. An interword space stretches by 1/2 and shrinks by 1/3 of its width; CJK break opportunities
// stretch the same but never shrink. Ragged alignments keep spaces fixed and measure each line against a 2 em
// rag allowance like \raggedright, so hyphenation is only used where it saves a deep notch.
constexpr int LINE_PENALTY = 10;                   // \linepenalty
constexpr int HYPHEN_PENALTY = 50;                 // \hyphenpenalty, \exhyphenpenalty
constexpr int64_t DOUBLE_HYPHEN_DEMERITS = 10000;  // \doublehyphendemerits
constexpr int64_t FINAL_HYPHEN_DEMERITS = 5000;    // \finalhyphendemerits
constexpr int64_t ADJ_DEMERITS = 10000;            // \adjdemerits
// Ragged lines have no gaps to even out, so a hyphen there only shortens the rag: a third fewer hyphens than at
// HYPHEN_PENALTY for a rag barely deeper.
constexpr int RAGGED_HYPHEN_PENALTY = 300;
// More than MAX_HYPHEN_RUN hyphenated lines in a row: a ladder down the margin, taken only to avoid a line looser
// than about badness 3000.
constexpr uint8_t MAX_HYPHEN_RUN = 2;
constexpr int64_t HYPHEN_LADDER_DEMERITS = 10000000;
constexpr int64_t UNREACHED = std::numeric_limits<int64_t>::max();
// TeX rejects lines past badness 10000 and retries with \emergencystretch. Badness here keeps growing past that
// point instead, so the least loose of several awful lines (narrow columns, large type) still wins.
constexpr int MAX_BAD = 10000000;
constexpr int64_t OVERFULL_DEMERITS = 1000000000000000;  // a lone piece wider than the line: last resort
// Stretch is measured in half pixels so a space's 1/2-width stretch stays integral. Shrink is a whole number of
// pixels per space (space / 3, rounded down) so that no single gap ever renders narrower than 2/3 of a space.
constexpr int GLUE_SCALE = 2;
constexpr int STRETCH_PER_SPACE = GLUE_SCALE / 2;
constexpr int RAGGED_STRETCH_SPACES = 6;  // ~2 em of interword spaces

enum LineFitness : uint8_t { VERY_LOOSE = 0, LOOSE = 1, DECENT = 2, TIGHT = 3 };

// TeX's badness (tex.web section 108), about 100 * (t / s)^3 for s > 0, in 32-bit integer arithmetic up to
// TeX's 10000 ceiling (the ESP32-C3 has hardware 32-bit multiply and divide but no FPU), then capped at MAX_BAD.
int badness(const int t, const int s) {
  if (t <= 0) return 0;
  int r;
  if (t <= 7230584) {
    r = t * 297 / s;
  } else if (s >= 1663497) {
    r = t / (s / 297);
  } else {
    r = t;
  }
  if (r <= 1290) return (r * r * r + 0x20000) / 0x40000;
  if (r >= 13800) return MAX_BAD;  // 13800^3 / 2^18 > MAX_BAD
  const int64_t cube = static_cast<int64_t>(r) * r * r;
  return static_cast<int>(std::min<int64_t>(MAX_BAD, (cube + 0x20000) / 0x40000));
}

int64_t addDemerits(const int64_t total, const int64_t line) {
  return total >= UNREACHED - 1 - line ? UNREACHED - 1 : total + line;
}

constexpr uint8_t BREAK_INSERTS_HYPHEN = 0x01;  // the line gains a visible '-' when broken here
constexpr uint8_t BREAK_PENALIZED = 0x02;       // splits a word: costs HYPHEN_PENALTY
constexpr uint8_t BREAK_FLAGGED = 0x04;         // the line ends in a hyphen: counts toward double-hyphen demerits
constexpr uint8_t BREAK_FORCED = 0x08;          // every line reaching this break must end here
constexpr uint8_t BREAK_FITNESS_SHIFT = 4;      // LineFitness of the best line ending here (2 bits)
constexpr uint8_t BREAK_RUN_SHIFT = 6;          // hyphenated lines in a row ending here, capped at 3 (2 bits)

// A place where a line may end: after token `token` (offset 0) or inside it, before byte `offset`.
struct LineBreakCandidate {
  uint16_t token;
  uint16_t prefixWidth;  // inside a token: the text before the break plus any inserted hyphen
  uint16_t suffixWidth;  // inside a token: the remainder that starts the next line
  uint16_t prev;         // best preceding candidate
  uint8_t offset;
  uint8_t flags;

  uint8_t fitness() const { return (flags >> BREAK_FITNESS_SHIFT) & 3; }
  void setFitness(const uint8_t fitness) {
    flags = static_cast<uint8_t>((flags & ~(3u << BREAK_FITNESS_SHIFT)) | (fitness << BREAK_FITNESS_SHIFT));
  }
  uint8_t hyphenRun() const { return flags >> BREAK_RUN_SHIFT; }
  void setHyphenRun(const uint8_t run) {
    flags = static_cast<uint8_t>((flags & ~(3u << BREAK_RUN_SHIFT)) | (std::min<uint8_t>(run, 3) << BREAK_RUN_SHIFT));
  }
};

static_assert(sizeof(LineBreakCandidate) == 10, "keep break candidates compact: one per word and hyphenation point");

// Growable candidate array on nothrow allocations: a paragraph whose break tables do not fit in the heap falls
// back to first-fit breaking instead of aborting. Indices must stay below UINT16_MAX (the prev field).
class CandidateList {
 public:
  bool reserve(const size_t capacity) {
    if (capacity <= cap) return true;
    auto grown = makeUniqueNoThrow<LineBreakCandidate[]>(capacity);
    if (!grown) return false;
    if (count > 0) memcpy(grown.get(), data.get(), count * sizeof(LineBreakCandidate));
    data = std::move(grown);
    cap = capacity;
    return true;
  }
  bool push(const LineBreakCandidate& candidate) {
    if (count >= UINT16_MAX - 1) return false;
    if (count == cap && !reserve(std::min<size_t>(cap + cap / 2 + 16, UINT16_MAX))) return false;
    data[count++] = candidate;
    return true;
  }
  size_t size() const { return count; }
  LineBreakCandidate& operator[](const size_t i) { return data[i]; }

 private:
  std::unique_ptr<LineBreakCandidate[]> data;
  size_t count = 0;
  size_t cap = 0;
};

// Width of one piece of a split token as drawn alone at a line edge: soft hyphens dropped, plus '-' when the break
// inserts one. Pieces are unterminated views into the word arena; plain ones are measured through a stack copy so
// the hot candidate loop does not allocate.
constexpr size_t SPLIT_PIECE_BUF_SIZE = 208;  // MAX_WORD_SIZE (200) + '-' + NUL, rounded up
uint16_t measureSplitPiece(const GfxRenderer& renderer, const int fontId, const std::string_view piece,
                           const EpdFontFamily::Style style, const uint8_t focusBoundary, const int8_t characterSpacing,
                           const uint8_t wordSpacingPercent, const bool appendHyphen) {
  if (focusBoundary != 0 || piece.size() + 2 > SPLIT_PIECE_BUF_SIZE) {
    return measureFocusWordWidth(renderer, fontId, piece, style, focusBoundary, characterSpacing, wordSpacingPercent,
                                 appendHyphen);
  }
  char buf[SPLIT_PIECE_BUF_SIZE];
  size_t len = 0;
  for (size_t i = 0; i < piece.size(); ++i) {
    if (piece[i] == SOFT_HYPHEN_UTF8[0] && i + 1 < piece.size() && piece[i + 1] == SOFT_HYPHEN_UTF8[1]) {
      ++i;
      continue;
    }
    buf[len++] = piece[i];
  }
  if (appendHyphen) buf[len++] = '-';
  buf[len] = '\0';
  return static_cast<uint16_t>(renderer.getTextAdvanceX(fontId, buf, style, characterSpacing));
}

// Cheap pre-filter for Hyphenator::visibleHyphenBreakOffsets(): ASCII '-' or '_', or a lead byte shared by every
// non-ASCII character isExplicitHyphen() accepts (U+058A, U+2010..U+2E3B, U+FE58..U+FF0D).
bool mayContainVisibleHyphen(const std::string_view word) {
  return std::any_of(word.begin(), word.end(), [](const char c) {
    const auto byte = static_cast<uint8_t>(c);
    return byte == '-' || byte == '_' || byte == 0xD6 || byte == 0xE2 || byte == 0xEF;
  });
}

}  // namespace

bool ParsedText::isBlank() const {
  for (size_t i = 0; i < words.size(); ++i) {
    const std::string_view word = wordAt(i);
    if (word.find_first_not_of(' ') != std::string_view::npos) return false;
  }
  return true;
}

uint32_t ParsedText::visibleOffsetBaseAt(const size_t wordIndex) const {
  uint32_t base = visibleOffsetBase;
  for (const auto& rebase : visibleOffsetRebases) {
    if (rebase.wordIndex > wordIndex) break;
    base = rebase.base;
  }
  return base;
}

uint32_t ParsedText::visibleOffsetAt(const size_t wordIndex) const {
  if (wordIndex >= wordVisibleOffsetDeltas.size()) return 0;
  return visibleOffsetBaseAt(wordIndex) + wordVisibleOffsetDeltas[wordIndex];
}

void ParsedText::pushVisibleOffset(const uint32_t offset) {
  uint32_t base = visibleOffsetBase;
  if (wordVisibleOffsetDeltas.empty()) {
    visibleOffsetBase = offset;
    base = offset;
  } else if (!visibleOffsetRebases.empty()) {
    base = visibleOffsetRebases.back().base;
  }

  if (offset < base || offset - base > std::numeric_limits<uint16_t>::max()) {
    visibleOffsetRebases.push_back({wordVisibleOffsetDeltas.size(), offset});
    base = offset;
  }
  wordVisibleOffsetDeltas.push_back(static_cast<uint16_t>(offset - base));
}

void ParsedText::insertVisibleOffset(const size_t wordIndex, const uint32_t offset) {
  const uint32_t base = wordIndex > 0 ? visibleOffsetBaseAt(wordIndex - 1) : visibleOffsetBase;
  for (auto& rebase : visibleOffsetRebases) {
    if (rebase.wordIndex >= wordIndex) rebase.wordIndex++;
  }

  uint32_t insertionBase = base;
  if (offset < base || offset - base > std::numeric_limits<uint16_t>::max()) {
    const auto rebaseIt = std::find_if(visibleOffsetRebases.begin(), visibleOffsetRebases.end(),
                                       [wordIndex](const auto& rebase) { return rebase.wordIndex > wordIndex; });
    visibleOffsetRebases.insert(rebaseIt, {wordIndex, offset});
    insertionBase = offset;
  }
  wordVisibleOffsetDeltas.insert(wordVisibleOffsetDeltas.begin() + wordIndex,
                                 static_cast<uint16_t>(offset - insertionBase));
}

void ParsedText::eraseVisibleOffsetPrefix(const size_t count) {
  if (count >= wordVisibleOffsetDeltas.size()) {
    wordVisibleOffsetDeltas.clear();
    visibleOffsetRebases.clear();
    visibleOffsetBase = 0;
    return;
  }

  const uint32_t newBase = visibleOffsetBaseAt(count);
  wordVisibleOffsetDeltas.erase(wordVisibleOffsetDeltas.begin(), wordVisibleOffsetDeltas.begin() + count);
  size_t writeIndex = 0;
  for (auto rebase : visibleOffsetRebases) {
    if (rebase.wordIndex <= count) continue;
    rebase.wordIndex -= count;
    visibleOffsetRebases[writeIndex++] = rebase;
  }
  visibleOffsetRebases.resize(writeIndex);
  visibleOffsetBase = newBase;
}

bool ParsedText::storeWord(const std::string_view text, WordStore::StoredWord& out) {
  if (wordStore.append(text.data(), text.size(), out)) return true;
  if (!droppedWords) {
    LOG_ERR("PTX", "OOM: dropping paragraph text (arena chunk alloc failed)");
  }
  droppedWords = true;
  return false;
}

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, const uint32_t visibleTextOffset, const uint8_t linkId) {
  if (word.empty()) return;

  // The device fonts carry no combining-mark positioning, so EPUB text stored in NFD
  // (a base letter followed by separate combining accents -- common for Vietnamese,
  // and used for many EPUB <h1> chapter headings) renders with the marks detached or
  // misplaced. Compose to NFC here, the single funnel every word passes through, so a
  // precomposed glyph is used instead. This runs once per word at layout time (the
  // result is cached in the section file) and is a cheap no-op for mark-free text.
  word = utf8ComposeNfc(word);

  EpdFontFamily::Style baseStyle = fontStyle;
  if (underline) {
    baseStyle = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::UNDERLINE);
  }
  const bool wordStartsRtl = !hasRtlWord && mayContainRtlBytes(word.c_str()) &&
                             BidiUtils::startsWithRtl(word.c_str(), RTL_PER_WORD_PROBE_DEPTH);

  // All token pushes funnel through here: the arena append is the only
  // fallible step, and a failed append drops the token without touching the
  // parallel arrays (they must stay in lockstep with words).
  const auto pushStyledToken = [&](std::string_view token, const EpdFontFamily::Style style, const bool continues,
                                   const bool noSpaceBefore, const uint8_t focusBoundary, const uint32_t tokenOffset,
                                   const bool padRuby) {
    WordStore::StoredWord stored;
    if (!storeWord(token, stored)) return;
    words.push_back(stored);
    wordStyles.push_back(style);
    wordContinues.push_back(continues);
    wordNoSpaceBefore.push_back(noSpaceBefore);
    wordFocusBoundary.push_back(focusBoundary);
    wordLinkIds.push_back(linkId);
    pushVisibleOffset(tokenOffset);
    if (padRuby && !rubyTexts.empty()) {
      rubyTexts.push_back("");
    }
  };

  const auto pushToken = [&](std::string_view token, const bool continues, const bool noSpaceBefore,
                             const uint8_t focusBoundary, const uint32_t tokenOffset) {
    pushStyledToken(token, baseStyle, continues, noSpaceBefore, focusBoundary, tokenOffset, true);
  };

  bool effectiveAttachToPrevious = attachToPrevious;
  bool effectiveNoSpaceBefore = false;
  // Only a glued token (attachToPrevious == true, i.e. no whitespace separated it from the
  // previous one in the source) may be turned into a gap-less break opportunity. When real
  // whitespace separated the two words, that space is content and must be rendered: Korean
  // is a space-delimited script written in Hangul, which utf8IsCjkBreakable() covers.
  if (attachToPrevious && !words.empty() &&
      hasCjkBreakOpportunityBetween(lastCodepoint(wordStore.view(words.back())), firstCodepoint(word))) {
    effectiveAttachToPrevious = false;
    effectiveNoSpaceBefore = true;
  }

  // Bulk-reserve the per-token parallel arrays before a burst of pushes so they
  // don't repeatedly double. Only the std::vector arrays are reserved: words and
  // rubyTexts are std::deque (chunked growth, no reserve()/capacity() and no large
  // contiguous reallocation to avoid). wordStyles' capacity gauges them all since
  // pushToken() keeps every array in lockstep.
  const auto ensureTokenCapacity = [&](const size_t additionalTokens) {
    if (additionalTokens == 0) return;
    const size_t requiredSize = words.size() + additionalTokens;
    if (wordStyles.capacity() >= requiredSize) return;

    size_t newCapacity = wordStyles.capacity() < 16 ? 16 : wordStyles.capacity();
    while (newCapacity < requiredSize) {
      newCapacity *= 2;
    }

    wordStyles.reserve(newCapacity);
    wordContinues.reserve(newCapacity);
    wordNoSpaceBefore.reserve(newCapacity);
    wordFocusBoundary.reserve(newCapacity);
    wordLinkIds.reserve(newCapacity);
    wordVisibleOffsetDeltas.reserve(newCapacity);
  };

  if (auto breakOffsets = cjkCharacterBreakByteOffsets(word); !breakOffsets.empty()) {
    // CJK-heavy paragraphs can push hundreds of tiny tokens quickly when CSS toggles
    // inline styles. Reserve once up front to avoid repeated vector growth reallocations.
    ensureTokenCapacity(breakOffsets.size() + 1);
    bool firstToken = true;
    size_t tokenStart = 0;
    uint32_t tokenVisibleOffset = visibleTextOffset;
    for (const size_t breakOffset : breakOffsets) {
      if (breakOffset <= tokenStart || breakOffset > word.size()) continue;
      const std::string_view token(word.data() + tokenStart, breakOffset - tokenStart);
      pushToken(token, firstToken ? effectiveAttachToPrevious : false, firstToken ? effectiveNoSpaceBefore : true,
                /*focusBoundary=*/0, tokenVisibleOffset);
      tokenVisibleOffset += countCodepoints(token);
      firstToken = false;
      tokenStart = breakOffset;
    }
    if (tokenStart < word.size()) {
      pushToken(std::string_view(word).substr(tokenStart), firstToken ? effectiveAttachToPrevious : false,
                firstToken ? effectiveNoSpaceBefore : true, /*focusBoundary=*/0, tokenVisibleOffset);
    }
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  if (containsCjkBreakableCodepoint(word)) {
    pushToken(word, effectiveAttachToPrevious, effectiveNoSpaceBefore, /*focusBoundary=*/0, visibleTextOffset);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // Already-bold text should stay fully bold; focus splitting would make its suffix regular later.
  if (!this->focusReadingEnabled || (baseStyle & EpdFontFamily::BOLD) != 0) {
    pushToken(word, effectiveAttachToPrevious, effectiveNoSpaceBefore, /*focusBoundary=*/0, visibleTextOffset);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // --- FOCUS READING LOGIC BELOW ---

  // Worst case: a segment boundary on each byte (highly punctuated UTF-8 text).
  ensureTokenCapacity(word.length());

  // Lambda helper to process and push individual sub-segments of the string
  // Use std::string_view to avoid heap allocations when slicing
  auto processSegment = [&](std::string_view segment, bool isWord, bool attach, bool noSpaceBefore) {
    const unsigned char* wordBegin = reinterpret_cast<const unsigned char*>(word.data());
    const unsigned char* segmentBegin = reinterpret_cast<const unsigned char*>(segment.data());
    uint32_t segmentOffset = visibleTextOffset;
    const unsigned char* offsetPtr = wordBegin;
    while (offsetPtr < segmentBegin) {
      utf8NextCodepoint(&offsetPtr);
      segmentOffset++;
    }
    if (!isWord) {
      // Punctuation and Numbers stay regular
      pushStyledToken(segment, baseStyle, attach, noSpaceBefore, /*focusBoundary=*/0, segmentOffset, false);
    } else {
      size_t charCount = 0;
      const unsigned char* countPtr = reinterpret_cast<const unsigned char*>(segment.data());
      const unsigned char* countEnd = countPtr + segment.length();

      while (countPtr < countEnd) {
        utf8NextCodepoint(&countPtr);
        charCount++;
      }

      // Target 45% for 1-bold at 4 chars and 3-bold at 7 chars with floor truncation
      constexpr size_t FOCUS_READING_PERCENT = 45;
      size_t targetBoldChars = (charCount * FOCUS_READING_PERCENT) / 100;
      targetBoldChars = std::clamp<size_t>(targetBoldChars, 1, 9);

      if (targetBoldChars >= charCount) {
        // Whole segment is bold - no suffix split needed
        pushStyledToken(segment, static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD), attach,
                        noSpaceBefore, /*focusBoundary=*/0, segmentOffset, false);
      } else {
        countPtr = reinterpret_cast<const unsigned char*>(segment.data());
        for (size_t i = 0; i < targetBoldChars; ++i) {
          utf8NextCodepoint(&countPtr);
        }
        size_t splitByteOffset = countPtr - reinterpret_cast<const unsigned char*>(segment.data());

        // One token carrying the emphasis as a byte boundary, so the word stays whole for the
        // hyphenator and the line breaker. The renderer applies BOLD to bytes [0, splitByteOffset).
        pushStyledToken(segment, baseStyle, attach, noSpaceBefore,
                        static_cast<uint8_t>(std::min<size_t>(splitByteOffset, 255)), segmentOffset, false);
      }
    }
  };

  // Tokenize the string by alternating states (Word vs. Non-Word)
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* end = ptr + word.length();

  const unsigned char* segmentStart = ptr;
  uint32_t firstCp = utf8NextCodepoint(&ptr);  // Consume the first char to determine initial state
  bool inWordSegment = isWordCharacter(firstCp);

  bool isFirstSegment = true;

  while (ptr < end) {
    const unsigned char* currentCpStart = ptr;
    uint32_t cp = utf8NextCodepoint(&ptr);
    bool isWordChar = isWordCharacter(cp);

    // Whenever the character type flips, slice off the segment we just completed and process it
    if (isWordChar != inWordSegment) {
      size_t segmentLen = currentCpStart - segmentStart;
      std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);

      // Only the very first segment inherits the original attachToPrevious flag.
      // Every subsequent segment glues seamlessly to the prefix. After a visible explicit-hyphen
      // character, continues=true + noSpaceBefore=true records a breakable attachment: it may wrap,
      // but when it stays on the line it receives kerning only, never a space or justification.
      const bool breakAfterPrev =
          !isFirstSegment && !words.empty() && endsWithBreakableHyphen(wordStore.view(words.back()));
      processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                     isFirstSegment ? effectiveNoSpaceBefore : breakAfterPrev);

      // Setup for the next segment
      segmentStart = currentCpStart;
      inWordSegment = isWordChar;
      isFirstSegment = false;
    }
  }

  // Process the final remaining segment
  size_t segmentLen = end - segmentStart;
  std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);
  const bool breakAfterPrev =
      !isFirstSegment && !words.empty() && endsWithBreakableHyphen(wordStore.view(words.back()));
  processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                 isFirstSegment ? effectiveNoSpaceBefore : breakAfterPrev);
  if (wordStartsRtl) {
    hasRtlWord = true;
  }
}

uint8_t ParsedText::addLinkTarget(const char* href) {
  if (!href || href[0] == '\0' || strnlen(href, FOOTNOTE_HREF_LEN) >= FOOTNOTE_HREF_LEN ||
      linkTargets.size() >= UINT8_MAX) {
    return 0;
  }
  linkTargets.emplace_back(href);
  return static_cast<uint8_t>(linkTargets.size());
}

bool ParsedText::linkTargetMatches(const uint8_t linkId, const char* href) const {
  return linkId > 0 && linkId <= linkTargets.size() && href && linkTargets[linkId - 1] == href;
}

void ParsedText::setRubyForWordAt(size_t index, const std::string& ruby) {
  if (index >= words.size()) return;
  if (rubyTexts.size() <= index) {
    rubyTexts.resize(words.size());
  }
  rubyTexts[index] = ruby;
}

void ParsedText::setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby) {
  if (startIndex >= words.size()) return;
  if (rubyTexts.size() <= startIndex) {
    rubyTexts.resize(words.size());
  }
  rubyTexts[startIndex] = ruby;
  for (size_t i = 1; i < count; i++) {
    size_t idx = startIndex + i;
    if (idx >= words.size()) break;
    if (rubyTexts.size() <= idx) {
      rubyTexts.resize(words.size());
    }
    rubyTexts[idx] = "";
    wordStyles[idx] =
        static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(wordStyles[idx]) | EpdFontFamily::RUBY_CONTINUE);
    wordContinues[idx] = true;       // Prevent page breaker from splitting the Group Ruby!
    wordNoSpaceBefore[idx] = false;  // Ensure allowsBreak returns false!
  }
}

void ParsedText::ensureRubyCapacity() {
  // No-op: rubyTexts is a std::deque (chunked growth, no capacity to pre-reserve
  // and no large contiguous reallocation to avoid). Kept for call-site stability.
}

int ParsedText::resolveFirstLineIndent(const bool isFirstLine, const GfxRenderer& renderer, const int fontId) const {
  // Text after a <br> continues its paragraph on a new line, which is never indented.
  if (!isFirstLine || !isNaturalAlign || blockStyle.fromBrElement) {
    return 0;
  }
  if (blockStyle.textIndentDefined) {
    if (blockStyle.textIndent < 0 || !extraParagraphSpacing) {
      return blockStyle.textIndent;
    }
    return 0;
  }
  if (!extraParagraphSpacing) {
    return interwordSpace(renderer, fontId) * 3;
  }
  return 0;
}

// The paragraph's nominal word space: the unit for first-line indents and for how far a justified space may
// stretch or shrink.
int ParsedText::interwordSpace(const GfxRenderer& renderer, const int fontId) const {
  return scaleSpace(renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR), wordSpacingPercent);
}

// Natural advance between words[wordIndex - 1] and words[wordIndex] on one line, as extractLine places them.
int ParsedText::naturalGapBefore(const size_t wordIndex, const GfxRenderer& renderer, const int fontId) const {
  if (wordNoSpaceBefore[wordIndex] && !wordContinues[wordIndex]) {
    return blockStyle.characterSpacing;
  }
  const uint32_t left = lastCodepoint(wordAt(wordIndex - 1));
  const uint32_t right = firstCodepoint(wordAt(wordIndex));
  if (wordContinues[wordIndex]) {
    // Attached and breakable-attached boundaries both use kerning when kept on one line.
    return renderer.getKerning(fontId, left, right, wordStyles[wordIndex - 1], blockStyle.characterSpacing);
  }
  return scaleSpace(renderer.getSpaceAdvance(fontId, left, right, wordStyles[wordIndex - 1]), wordSpacingPercent);
}
// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                                       const bool includeLastLine, const int8_t characterSpacing,
                                       const uint8_t wordSpacingPercent) {
  if (words.empty()) {
    return;
  }
  // Stamped here rather than at construction: the parser replaces blockStyle as CSS resolves.
  blockStyle.characterSpacing = characterSpacing;
  this->wordSpacingPercent = wordSpacingPercent;

  // Per-paragraph RTL auto-detection: only when CSS/HTML didn't explicitly set direction.
  // Explicit dir="ltr" must be respected and not overridden by content heuristic.
  if (!blockStyle.directionDefined && hasRtlWord) {
    // Check the first few words for RTL letter codepoints (no heap allocation).
    const size_t wordsToScan = std::min(words.size(), RTL_PARAGRAPH_PROBE_WORDS);
    for (size_t i = 0; i < wordsToScan; ++i) {
      if (BidiUtils::startsWithRtl(wordStore.cstr(words[i]), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH)) {
        blockStyle.isRtl = true;
        break;
      }
    }
  }

  isNaturalAlign =
      blockStyle.alignment == CssTextAlign::Justify ||
      (blockStyle.isRtl ? blockStyle.alignment == CssTextAlign::Right : blockStyle.alignment == CssTextAlign::Left);

  // Ensure SD card font glyph metrics are loaded before measuring word widths.
  // For flash-based fonts isSdCardFont() returns false and this block is skipped
  // entirely — no heap allocation. For SD card fonts this reads glyph metadata
  // (advanceX only, no bitmaps) for all unique codepoints in this paragraph so
  // that calculateWordWidths() can measure text without on-demand SD I/O.
  if (renderer.isSdCardFont(fontId)) {
    // Style mask: only ask the SD font to load advances for styles actually
    // used in this paragraph. Style index is the low two bits (regular/bold/
    // italic/bold-italic); the underline bit is irrelevant to advance metrics.
    uint8_t styleMask = 0;
    for (auto s : wordStyles) {
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(s) & 0x03));
    }
    if (styleMask == 0) styleMask = 0x01;  // defensive: regular only
    // Hand the arena chunks over as packed NUL-separated word runs. Two small
    // pointer tables (~8 B per live chunk) instead of per-word iteration.
    std::vector<const char*> segments;
    std::vector<size_t> segmentLens;
    segments.reserve(wordStore.chunkCount());
    segmentLens.reserve(wordStore.chunkCount());
    for (size_t i = 0; i < wordStore.chunkCount(); ++i) {
      const char* data = wordStore.chunkData(i);
      if (!data) continue;  // retired chunk
      segments.push_back(data);
      segmentLens.push_back(wordStore.chunkUsed(i));
    }
    renderer.ensureSdCardFontReady(fontId, segments.data(), segmentLens.data(), segments.size(), words.size() > 1,
                                   hyphenationEnabled, styleMask);
  }

  const int pageWidth = viewportWidth;
  auto wordWidths = calculateWordWidths(renderer, fontId);

  const std::vector<size_t> lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, wordWidths);
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore, lineBreakIndices, processLine, renderer,
                fontId);
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    firstLineExtracted = true;
    const size_t consumed = lineBreakIndices[lineCount - 1];
    for (size_t i = 0; i < consumed; ++i) {
      wordStore.release(words[i]);  // retires arena chunks as lines are consumed
    }
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
    wordNoSpaceBefore.erase(wordNoSpaceBefore.begin(), wordNoSpaceBefore.begin() + consumed);
    wordFocusBoundary.erase(wordFocusBoundary.begin(), wordFocusBoundary.begin() + consumed);
    wordLinkIds.erase(wordLinkIds.begin(), wordLinkIds.begin() + consumed);
    eraseVisibleOffsetPrefix(consumed);
    if (!rubyTexts.empty()) {
      const size_t rtConsumed = std::min(consumed, rubyTexts.size());
      rubyTexts.erase(rubyTexts.begin(), rubyTexts.begin() + rtConsumed);
    }
  }
}

static inline bool isCjkIdeograph(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x3FFFF);
}

// The first word of a line may have its ruby characters wider than the word (the base text). In that case, we need to
// move the base text to the right a bit so that ruby text doesn't overflow the left border, and it is still centered
// over the base text. This function calculates how much we need to move the base text to the right.
int ParsedText::calculateRubyExtraStartOffset(const size_t wordIdx, const size_t maxWordIdx,
                                              const GfxRenderer& renderer, const int fontId) const {
  if (rubyTexts.empty() || wordIdx >= rubyTexts.size() || rubyTexts[wordIdx].empty() ||
      (wordStyles[wordIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    return 0;
  }

  size_t groupWordCount = 1;
  while (wordIdx + groupWordCount < maxWordIdx &&
         (wordStyles[wordIdx + groupWordCount] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    groupWordCount++;
  }
  int groupActualWidth = 0;
  for (size_t k = 0; k < groupWordCount; ++k) {
    groupActualWidth += measureWordWidth(renderer, fontId, wordAt(wordIdx + k), wordStyles[wordIdx + k],
                                         blockStyle.characterSpacing, wordSpacingPercent);
  }
  const int rubyWidth =
      renderer.getTextAdvanceX(fontId, rubyTexts[wordIdx].c_str(), EpdFontFamily::SUP, blockStyle.characterSpacing);
  if (rubyWidth <= groupActualWidth) {
    return 0;
  }

  const int leftOverlap = (rubyWidth - groupActualWidth) / 2;

  // This function is only ever called for the first word of a line.
  // words[wordIdx - 1], if it exists, is always the last word of the *prior* line
  // and cannot absorb any left overhang on the current line.
  // The full leftOverlap must therefore be reserved as a visual indent so the
  // ruby text does not overflow the left margin.
  return leftOverlap;
}

// The last ruby group on a line may have its ruby characters wider than the group's base text.
// The right half of that overhang protrudes past the last base character. This function returns
// the amount of right-margin space that must be reserved so the ruby does not overflow the right
// border. It mirrors calculateRubyExtraStartOffset: words[lineBreak] is on the *next* line and
// cannot absorb any of the right overhang on the current line, so the full rightOverlap is returned.
int ParsedText::calculateRubyExtraEndOffset(const size_t lineStartIdx, const size_t lineBreakIdx,
                                            const GfxRenderer& renderer, const int fontId) const {
  if (rubyTexts.empty() || lineBreakIdx == 0 || lineStartIdx >= lineBreakIdx) {
    return 0;
  }

  // Walk backwards from the last word to find the leader of the last ruby group on the line.
  size_t leaderIdx = lineBreakIdx - 1;
  while (leaderIdx > lineStartIdx && (wordStyles[leaderIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    leaderIdx--;
  }

  // leaderIdx must be a ruby group leader (non-empty ruby, no RUBY_CONTINUE flag).
  if (leaderIdx >= rubyTexts.size() || rubyTexts[leaderIdx].empty() ||
      (wordStyles[leaderIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    return 0;
  }

  // Measure the group.
  int groupActualWidth = 0;
  for (size_t k = leaderIdx; k < lineBreakIdx; ++k) {
    groupActualWidth +=
        measureWordWidth(renderer, fontId, wordAt(k), wordStyles[k], blockStyle.characterSpacing, wordSpacingPercent);
  }
  const int rubyWidth =
      renderer.getTextAdvanceX(fontId, rubyTexts[leaderIdx].c_str(), EpdFontFamily::SUP, blockStyle.characterSpacing);
  if (rubyWidth <= groupActualWidth) {
    return 0;
  }

  return (rubyWidth - groupActualWidth) / 2;
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureFocusWordWidth(renderer, fontId, wordAt(i), wordStyles[i], wordFocusBoundary[i],
                                               blockStyle.characterSpacing, wordSpacingPercent));
  }

  // Adjust widths for ruby groups to comply with JLReq standards
  if (!rubyTexts.empty()) {
    struct RubyGroupInfo {
      size_t start;
      size_t count;
      int baseWidth;
      int rubyWidth;
      int leftOverlap;
      int rightOverlap;
    };

    std::vector<RubyGroupInfo> groups;
    for (size_t i = 0; i < words.size(); ++i) {
      if (i < rubyTexts.size() && !rubyTexts[i].empty() && (wordStyles[i] & EpdFontFamily::RUBY_CONTINUE) == 0) {
        RubyGroupInfo g;
        g.start = i;
        g.baseWidth = wordWidths[i];
        g.count = 1;
        while (i + g.count < words.size() && (wordStyles[i + g.count] & EpdFontFamily::RUBY_CONTINUE) != 0) {
          g.baseWidth += wordWidths[i + g.count];
          g.count++;
        }
        g.rubyWidth =
            renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP, blockStyle.characterSpacing);
        g.leftOverlap = std::max(0, (g.rubyWidth - g.baseWidth) / 2);
        g.rightOverlap = std::max(0, (g.rubyWidth - g.baseWidth) / 2);
        groups.push_back(g);
        i += g.count - 1;
      }
    }

    // Adjust widths based on adjacent characters and group-to-group spacing
    for (size_t gIdx = 0; gIdx < groups.size(); ++gIdx) {
      const auto& g = groups[gIdx];

      // 1. Preceding character (left overhang)
      if (g.start > 0) {
        const uint32_t cpPrev = lastCodepoint(wordAt(g.start - 1));
        if (isCjkIdeograph(cpPrev)) {
          wordWidths[g.start - 1] += g.leftOverlap;
        } else {
          const int maxLeftOverhang = wordWidths[g.start - 1] / 2;
          wordWidths[g.start - 1] += std::max(0, g.leftOverlap - maxLeftOverhang);
        }
      }

      // 2. Succeeding character (right overhang / group collision)
      const size_t nextIdx = g.start + g.count;
      if (nextIdx < words.size()) {
        if (gIdx + 1 < groups.size() && groups[gIdx + 1].start == nextIdx) {
          // Adjacent ruby groups: compute collision
          const auto& nextG = groups[gIdx + 1];
          const int collision = g.rightOverlap + nextG.leftOverlap;
          if (collision > 0) {
            wordWidths[g.start + g.count - 1] += collision;
          }
        } else {
          // Regular character following: check if it's Kanji
          const uint32_t cpNext = firstCodepoint(wordAt(nextIdx));
          if (isCjkIdeograph(cpNext)) {
            wordWidths[g.start + g.count - 1] += g.rightOverlap;
          } else {
            const int maxRightOverhang = wordWidths[nextIdx] / 2;
            wordWidths[g.start + g.count - 1] += std::max(0, g.rightOverlap - maxRightOverhang);
          }

          // Check if there is another ruby group further ahead separated only by non-ideographs
          if (gIdx + 1 < groups.size()) {
            const auto& nextG = groups[gIdx + 1];
            bool onlyNonIdeographsInBetween = true;
            int gapWidth = 0;
            for (size_t k = nextIdx; k < nextG.start; ++k) {
              const uint32_t cp = firstCodepoint(wordAt(k));
              if (isCjkIdeograph(cp)) {
                onlyNonIdeographsInBetween = false;
                break;
              }
              gapWidth += wordWidths[k];
            }
            if (onlyNonIdeographsInBetween) {
              const int maxRightOverhang = wordWidths[g.start + g.count - 1] / 2;
              const int maxLeftOverhang = wordWidths[nextG.start - 1] / 2;
              const int allowedRight = std::min(g.rightOverlap, maxRightOverhang);
              const int allowedLeft = std::min(nextG.leftOverlap, maxLeftOverhang);
              const int touchOverlap = allowedRight + allowedLeft - gapWidth;
              if (touchOverlap > 0) {
                wordWidths[g.start + g.count - 1] += touchOverlap;
              }
            }
          }
        }
      }
    }
  }

  return wordWidths;
}

// Chooses line breaks for the whole paragraph at once (Knuth-Plass total fit) rather than line by line: every
// word space, CJK break opportunity, visible hyphen and -- with hyphenation on -- hyphenation point is a candidate,
// and the set of breaks with the least total demerits wins. A loose line is weighed against its neighbours, so a
// slightly tighter line above can spare a gaping one below, and a hyphen is only used where it pays for itself.
std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  std::vector<uint16_t>& wordWidths) {
  if (words.empty()) {
    return {};
  }

  const int firstLineIndent = resolveFirstLineIndent(!firstLineExtracted, renderer, fontId);

  // Places inside a word where a line may end: hyphenation points with hyphenation on, visible hyphens and
  // dashes always. Sorted by offset; at a shared offset the break that inserts no hyphen wins.
  const auto splitPoints = [this](const std::string_view view) {
    std::vector<Hyphenator::BreakInfo> points;
    // A Hangul split needs two syllables and any Hyphenator break three codepoints (letter, hyphen, letter), so
    // shorter tokens skip the codepoint copies and trie walk.
    const uint32_t codepointCount = countCodepoints(view);
    if (codepointCount < 2) return points;
    const std::string word{view};  // brace-init: `word(` is an Arduino macro
    if (!hyphenationEnabled) {
      if (mayContainVisibleHyphen(view)) points = Hyphenator::visibleHyphenBreakOffsets(word);
      return points;
    }
    points = hangulLineEndBreaks(word);
    if (codepointCount >= 3) {
      const auto patternPoints = Hyphenator::breakOffsets(word, /*includeFallback=*/false);
      points.insert(points.end(), patternPoints.begin(), patternPoints.end());
    }
    std::sort(points.begin(), points.end(), [](const Hyphenator::BreakInfo& a, const Hyphenator::BreakInfo& b) {
      return a.byteOffset != b.byteOffset ? a.byteOffset < b.byteOffset
                                          : a.requiresInsertedHyphen < b.requiresInsertedHyphen;
    });
    points.erase(std::unique(points.begin(), points.end(),
                             [](const Hyphenator::BreakInfo& a, const Hyphenator::BreakInfo& b) {
                               return a.byteOffset == b.byteOffset;
                             }),
                 points.end());
    return points;
  };
  const auto pieceWidths = [&](const size_t t, const Hyphenator::BreakInfo& point, uint16_t& prefix, uint16_t& suffix) {
    const std::string_view view = wordAt(t);
    const size_t offset = point.byteOffset;
    prefix = measureSplitPiece(renderer, fontId, view.substr(0, offset), wordStyles[t],
                               focusBoundaryBefore(wordFocusBoundary[t], offset), blockStyle.characterSpacing,
                               wordSpacingPercent, point.requiresInsertedHyphen);
    suffix = measureSplitPiece(renderer, fontId, view.substr(offset), wordStyles[t],
                               focusBoundaryAfter(wordFocusBoundary[t], offset), blockStyle.characterSpacing,
                               wordSpacingPercent, false);
  };

  // A word wider than the line normally ends one line and starts the next; the DP below finds that split. Only a
  // word with no such split (longer than two lines, or no break point that fits) is cut here, at the widest prefix
  // that fits a line, with fallback breaks if need be. Those cuts are forced line breaks.
  std::vector<size_t> forcedBreaks;  // tokens that must start a line, ascending
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    if (wordWidths[i] <= effectiveWidth) continue;
    bool splitFits = false;
    for (const auto& point : splitPoints(wordAt(i))) {
      if (point.byteOffset == 0 || point.byteOffset >= wordAt(i).size() || point.byteOffset > UINT8_MAX) continue;
      uint16_t prefix;
      uint16_t suffix;
      pieceWidths(i, point, prefix, suffix);
      if (prefix <= effectiveWidth && suffix <= pageWidth) {
        splitFits = true;
        break;
      }
    }
    if (!splitFits &&
        hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
      forcedBreaks.push_back(i + 1);
    }
  }

  const size_t wordCount = words.size();
  if (wordCount >= UINT16_MAX - 1) {
    return computeGreedyLineBreaks(renderer, fontId, pageWidth, firstLineIndent, wordWidths);
  }

  // Per-boundary glue, computed once: the DP below revisits every boundary once per line start that reaches it.
  struct Glue {
    int16_t width;
    bool stretches;  // a justifiable gap: word spaces, no-break space tokens and CJK break opportunities
    bool shrinks;    // a justifiable gap that is a real space
  };
  auto glue = makeUniqueNoThrow<Glue[]>(wordCount);
  CandidateList candidates;
  // Every token end plus typically under two hyphenation points per word; the list grows if a paragraph has more.
  const size_t expectedCandidates = hyphenationEnabled ? 2 * wordCount + 2 : wordCount + wordCount / 8 + 2;
  if (!glue || !candidates.reserve(expectedCandidates)) {
    LOG_ERR("PTX", "OOM: line-break tables for %u words, using first-fit", static_cast<unsigned>(wordCount));
    return computeGreedyLineBreaks(renderer, fontId, pageWidth, firstLineIndent, wordWidths);
  }
  for (size_t t = 1; t < wordCount; ++t) {
    const bool spaceToken = wordAt(t) == " ";
    glue[t].width = static_cast<int16_t>(naturalGapBefore(t, renderer, fontId));
    glue[t].stretches = TokenBoundary::isJustifiableGap(wordContinues[t], wordNoSpaceBefore[t], spaceToken);
    glue[t].shrinks = glue[t].stretches && !wordNoSpaceBefore[t];
  }

  // Candidate 0 is the paragraph start; the rest are ordered by token, breaks inside a token before its end.
  bool tablesComplete = candidates.push(LineBreakCandidate{});
  size_t nextForced = 0;
  for (size_t t = 0; t < wordCount && tablesComplete; ++t) {
    const std::string_view view = wordAt(t);
    const bool endsForced = nextForced < forcedBreaks.size() && forcedBreaks[nextForced] == t + 1;
    if (endsForced) {
      ++nextForced;
    } else {
      // A cut prefix already fits its line; splitting it again would strand a fragment before the forced break.
      for (const auto& point : splitPoints(view)) {
        const size_t offset = point.byteOffset;
        if (offset == 0 || offset >= view.size() || offset > UINT8_MAX) continue;
        LineBreakCandidate candidate{};
        candidate.token = static_cast<uint16_t>(t);
        candidate.offset = static_cast<uint8_t>(offset);
        pieceWidths(t, point, candidate.prefixWidth, candidate.suffixWidth);
        candidate.flags = BREAK_PENALIZED;
        if (point.requiresInsertedHyphen) {
          candidate.flags |= BREAK_INSERTS_HYPHEN | BREAK_FLAGGED;
        } else if (endsWithBreakableHyphen(view.substr(0, offset))) {
          candidate.flags |= BREAK_FLAGGED;
        }
        if (!candidates.push(candidate)) {
          tablesComplete = false;
          break;
        }
      }
    }

    const bool lastToken = t + 1 == wordCount;
    if (tablesComplete && (lastToken || TokenBoundary::allowsBreak(wordContinues[t + 1], wordNoSpaceBefore[t + 1]))) {
      LineBreakCandidate candidate{};
      candidate.token = static_cast<uint16_t>(t);
      if (endsForced) {
        candidate.flags = BREAK_FORCED | (endsWithBreakableHyphen(view) ? BREAK_FLAGGED : 0);
      } else if (!lastToken && wordContinues[t + 1]) {
        // A breakable attachment (continues + noSpaceBefore) follows a visible hyphen or dash.
        candidate.flags = BREAK_PENALIZED | (endsWithBreakableHyphen(view) ? BREAK_FLAGGED : 0);
      }
      tablesComplete = candidates.push(candidate);
    }
  }

  const size_t candidateCount = candidates.size();
  auto demerits = tablesComplete ? makeUniqueNoThrow<int64_t[]>(candidateCount) : nullptr;
  if (!demerits) {
    LOG_ERR("PTX", "OOM: %u line-break candidates, using first-fit", static_cast<unsigned>(candidateCount));
    return computeGreedyLineBreaks(renderer, fontId, pageWidth, firstLineIndent, wordWidths);
  }

  const bool justify = blockStyle.alignment == CssTextAlign::Justify;
  const int64_t hyphenDemerits = justify ? static_cast<int64_t>(HYPHEN_PENALTY) * HYPHEN_PENALTY
                                         : static_cast<int64_t>(RAGGED_HYPHEN_PENALTY) * RAGGED_HYPHEN_PENALTY;
  const int space = std::max(1, interwordSpace(renderer, fontId));
  const int shrinkPerSpace = space / 3;
  const int raggedStretch = GLUE_SCALE * RAGGED_STRETCH_SPACES * space;
  const bool hasRuby = !rubyTexts.empty();

  for (size_t c = 1; c < candidateCount; ++c) demerits[c] = UNREACHED;
  demerits[0] = 0;
  candidates[0].setFitness(DECENT);

  // Forward DP: candidates are visited in order, so each one's best predecessor is final before it starts a line.
  for (size_t from = 0; from + 1 < candidateCount; ++from) {
    if (demerits[from] == UNREACHED) continue;
    const LineBreakCandidate start = candidates[from];
    const bool startsInsideToken = from != 0 && start.offset != 0;
    const size_t firstToken = from == 0 ? 0 : (startsInsideToken ? start.token : start.token + 1u);
    const int lineWidth = from == 0 ? pageWidth - firstLineIndent : pageWidth;

    int natural =
        (hasRuby && !startsInsideToken) ? calculateRubyExtraStartOffset(firstToken, wordCount, renderer, fontId) : 0;
    int stretchGaps = 0;
    int shrinkGaps = 0;
    size_t firstReachable = 0;
    bool anyFeasible = false;
    size_t c = from + 1;

    bool reachedForcedBreak = false;
    for (size_t t = firstToken; t < wordCount && c < candidateCount && !reachedForcedBreak; ++t) {
      int before = natural;
      if (t != firstToken) {
        before += glue[t].width;
        stretchGaps += glue[t].stretches;
        shrinkGaps += glue[t].shrinks;
      }
      const int pieceWidth = (t == firstToken && startsInsideToken) ? start.suffixWidth : wordWidths[t];

      for (; c < candidateCount && candidates[c].token == t; ++c) {
        const LineBreakCandidate& end = candidates[c];
        const bool endsInsideToken = end.offset != 0;
        if (endsInsideToken && t == firstToken && startsInsideToken) continue;  // never two breaks in one line's token
        if (firstReachable == 0) firstReachable = c;
        reachedForcedBreak = (end.flags & BREAK_FORCED) != 0;

        int width = before + (endsInsideToken ? end.prefixWidth : pieceWidth);
        if (hasRuby && !endsInsideToken) width += calculateRubyExtraEndOffset(firstToken, t + 1, renderer, fontId);

        const bool lastLine = c + 1 == candidateCount;
        const int shortfall = lineWidth - width;
        int bad = 0;
        uint8_t fitness = DECENT;
        if (shortfall >= 0) {
          if (lastLine) {
            bad = 0;  // the last line is set ragged, however short
          } else if (justify) {
            // A line with no gap to stretch is set ragged; its blank end reads like one widened gap.
            bad = badness(GLUE_SCALE * shortfall, std::max(stretchGaps, 1) * STRETCH_PER_SPACE * space);
            fitness = bad > 99 ? VERY_LOOSE : (bad > 12 ? LOOSE : DECENT);
          } else {
            bad = badness(GLUE_SCALE * shortfall, raggedStretch);
          }
        } else if (justify && !lastLine && -shortfall <= shrinkGaps * shrinkPerSpace) {
          bad = badness(GLUE_SCALE * -shortfall, GLUE_SCALE * shrinkGaps * shrinkPerSpace);
          if (bad > 12) fitness = TIGHT;
        } else {
          continue;  // overfull even with every space at its narrowest
        }

        int64_t lineDemerits = static_cast<int64_t>(LINE_PENALTY + bad) * (LINE_PENALTY + bad);
        if (end.flags & BREAK_PENALIZED) lineDemerits += hyphenDemerits;
        if ((start.flags & BREAK_FLAGGED) && (end.flags & BREAK_FLAGGED)) lineDemerits += DOUBLE_HYPHEN_DEMERITS;
        if (lastLine && (start.flags & BREAK_FLAGGED)) lineDemerits += FINAL_HYPHEN_DEMERITS;
        if (justify && std::abs(fitness - start.fitness()) > 1) lineDemerits += ADJ_DEMERITS;
        // Counted along each candidate's best path, like fitness, rather than kept per run length as TeX keeps
        // per-fitness active nodes.
        const uint8_t hyphenRun = (end.flags & BREAK_FLAGGED) ? start.hyphenRun() + 1 : 0;
        if (hyphenRun > MAX_HYPHEN_RUN) lineDemerits += HYPHEN_LADDER_DEMERITS;

        anyFeasible = true;
        // <= : on a tie the later start wins, keeping earlier lines full (avoids stray short CJK lines).
        const int64_t total = addDemerits(demerits[from], lineDemerits);
        if (total <= demerits[c]) {
          demerits[c] = total;
          candidates[c].prev = static_cast<uint16_t>(from);
          candidates[c].setFitness(fitness);
          candidates[c].setHyphenRun(hyphenRun);
        }
      }

      natural = before + pieceWidth;
      const int overflow = natural - lineWidth;
      if (firstReachable != 0 && overflow > 0 && (!justify || overflow > shrinkGaps * shrinkPerSpace)) {
        break;  // widths only grow from here
      }
    }

    // Nothing fits (an unsplittable piece wider than the line): take the shortest line and let it overflow.
    if (!anyFeasible && firstReachable != 0 &&
        addDemerits(demerits[from], OVERFULL_DEMERITS) <= demerits[firstReachable]) {
      demerits[firstReachable] = addDemerits(demerits[from], OVERFULL_DEMERITS);
      candidates[firstReachable].prev = static_cast<uint16_t>(from);
      candidates[firstReachable].setFitness(DECENT);
      candidates[firstReachable].setHyphenRun(0);
    }
  }

  size_t lineCount = 0;
  for (size_t c = candidateCount - 1; c != 0; c = candidates[c].prev) ++lineCount;
  std::vector<uint16_t> chosen(lineCount);
  for (size_t c = candidateCount - 1, i = lineCount; c != 0; c = candidates[c].prev) chosen[--i] = c;

  // Materialize breaks inside words; each split shifts the following token indices by one.
  std::vector<size_t> lineBreakIndices;
  lineBreakIndices.reserve(lineCount);
  size_t shift = 0;
  for (const uint16_t c : chosen) {
    const LineBreakCandidate& end = candidates[c];
    const size_t index = end.token + shift;
    if (end.offset != 0 && splitWordAt(index, end.offset, (end.flags & BREAK_INSERTS_HYPHEN) != 0, end.prefixWidth,
                                       end.suffixWidth, wordWidths)) {
      ++shift;
    }
    // A failed split (arena OOM) keeps the word whole on this line; the next break then collapses into this one.
    if (lineBreakIndices.empty() || index + 1 > lineBreakIndices.back()) {
      lineBreakIndices.push_back(index + 1);
    }
  }
  return lineBreakIndices;
}

// First-fit breaking at word boundaries only, for paragraphs whose line-break tables cannot be allocated.
std::vector<size_t> ParsedText::computeGreedyLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                        const int pageWidth, const int firstLineIndent,
                                                        const std::vector<uint16_t>& wordWidths) const {
  std::vector<size_t> lineBreakIndices;
  size_t lineStart = 0;
  size_t lastBreakable = 0;
  int lineWidth = wordWidths.empty() ? 0 : wordWidths[0];
  for (size_t t = 1; t < wordWidths.size(); ++t) {
    if (TokenBoundary::allowsBreak(wordContinues[t], wordNoSpaceBefore[t])) lastBreakable = t;
    lineWidth += naturalGapBefore(t, renderer, fontId) + wordWidths[t];
    const int available = lineBreakIndices.empty() ? pageWidth - firstLineIndent : pageWidth;
    if (lineWidth <= available || lastBreakable <= lineStart) continue;
    lineBreakIndices.push_back(lastBreakable);
    lineStart = lastBreakable;
    lineWidth = wordWidths[lineStart];
    for (size_t u = lineStart + 1; u <= t; ++u) lineWidth += naturalGapBefore(u, renderer, fontId) + wordWidths[u];
  }
  lineBreakIndices.push_back(wordWidths.size());
  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  // Stable copy: Hyphenator and the prefix measurements below want std::string
  // semantics. Bounded by one word.
  const std::string word{wordAt(wordIndex)};  // brace-init: `word(` is an Arduino macro
  const auto style = wordStyles[wordIndex];

  const uint8_t focusBoundary = wordFocusBoundary[wordIndex];

  // Collect candidate breakpoints (byte offsets and hyphen requirements). Focus emphasis is a byte
  // annotation, so the hyphenator sees the whole word and every legal break is reachable.
  // Hangul breaks come first so they win a tie against a hyphenated break at the same width.
  auto breakInfos = hangulLineEndBreaks(word);
  const auto hyphenBreaks = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  breakInfos.insert(breakInfos.end(), hyphenBreaks.begin(), hyphenBreaks.end());
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureSplitPiece(renderer, fontId, std::string_view(word).substr(0, offset), style,
                                              focusBoundaryBefore(focusBoundary, offset), blockStyle.characterSpacing,
                                              wordSpacingPercent, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  const uint16_t remainderWidth = measureSplitPiece(renderer, fontId, std::string_view(word).substr(chosenOffset),
                                                    style, focusBoundaryAfter(focusBoundary, chosenOffset),
                                                    blockStyle.characterSpacing, wordSpacingPercent, false);
  return splitWordAt(wordIndex, chosenOffset, chosenNeedsHyphen, static_cast<uint16_t>(chosenWidth), remainderWidth,
                     wordWidths);
}

// Splits words[wordIndex] before byte `byteOffset` into a prefix that ends a line (with a visible '-' when
// insertHyphen) and a remainder that starts the next one. The widths are those measured for the two pieces.
// Returns false, leaving the word whole, when the arena cannot hold the prefix.
bool ParsedText::splitWordAt(const size_t wordIndex, const size_t byteOffset, const bool insertHyphen,
                             const uint16_t prefixWidth, const uint16_t remainderWidth,
                             std::vector<uint16_t>& wordWidths) {
  const std::string_view word = wordAt(wordIndex);
  if (byteOffset == 0 || byteOffset >= word.size()) {
    return false;
  }
  const auto style = wordStyles[wordIndex];
  const uint8_t focusBoundary = wordFocusBoundary[wordIndex];

  uint32_t remainderOffset = visibleOffsetAt(wordIndex);
  const unsigned char* offsetPtr = reinterpret_cast<const unsigned char*>(word.data());
  const unsigned char* splitPtr = offsetPtr + byteOffset;
  while (offsetPtr < splitPtr) {
    utf8NextCodepoint(&offsetPtr);
    remainderOffset++;
  }

  // Split the word at the selected breakpoint. The prefix is materialized as a
  // fresh arena entry (with its visible hyphen, so it stays NUL-terminated);
  // the remainder aliases the original word's tail bytes and inherits the
  // original entry's release obligation via WordStore::suffix().
  // Stack buffer, not std::string: this runs mid-pagination, exactly when the
  // heap is under section-build pressure. The parser caps words at
  // MAX_WORD_SIZE (200) bytes; anything larger skips the split rather than
  // overflow (the word then breaks whole, as when no breakpoint fits).
  char prefixBuf[SPLIT_PIECE_BUF_SIZE];
  if (byteOffset + 1 > sizeof(prefixBuf)) {
    return false;
  }
  memcpy(prefixBuf, word.data(), byteOffset);
  size_t prefixLen = byteOffset;
  if (insertHyphen) {
    prefixBuf[prefixLen++] = '-';
  }
  WordStore::StoredWord prefixStored;
  if (!wordStore.append(prefixBuf, prefixLen, prefixStored)) {
    // OOM: skip the split; the word stays whole and the line breaks without it.
    return false;
  }
  const WordStore::StoredWord remainderStored = WordStore::suffix(words[wordIndex], byteOffset);
  words[wordIndex] = prefixStored;

  // Insert the remainder word (with matching style and continuation flag) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainderStored);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);
  insertVisibleOffset(wordIndex + 1, remainderOffset);
  // Emphasis follows the text across the split, so a break at or after the boundary leaves the
  // remainder fully regular.
  wordFocusBoundary.insert(wordFocusBoundary.begin() + wordIndex + 1, focusBoundaryAfter(focusBoundary, byteOffset));
  wordLinkIds.insert(wordLinkIds.begin() + wordIndex + 1, wordLinkIds[wordIndex]);
  wordFocusBoundary[wordIndex] = focusBoundaryBefore(focusBoundary, byteOffset);
  // Invariant: a boundary is always strictly inside its token, so an all-bold part carries BOLD in
  // its style with boundary 0 and nothing downstream special-cases boundary == size.
  // An inserted '-' counts as covered: measureFocusWordWidth() measured the
  // hyphen bold whenever the boundary spans the whole text before it, so the
  // rendered styling must match or justified spacing drifts by the bold/
  // regular hyphen advance delta.
  if (wordFocusBoundary[wordIndex] + (insertHyphen ? 1u : 0u) >= words[wordIndex].len) {
    wordStyles[wordIndex] = static_cast<EpdFontFamily::Style>(wordStyles[wordIndex] | EpdFontFamily::BOLD);
    wordFocusBoundary[wordIndex] = 0;
  }
  if (wordIndex + 1 <= rubyTexts.size()) {
    rubyTexts.insert(rubyTexts.begin() + wordIndex + 1, "");
  }

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This keeps the entire prefix group ("200 Quadrat-") on one line, while "kilometer"
  // moves to the next line.
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);
  wordNoSpaceBefore.insert(wordNoSpaceBefore.begin() + wordIndex + 1, false);

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = prefixWidth;
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
                             const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                             const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                             const GfxRenderer& renderer, const int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;
  const uint32_t lineVisibleOffset = visibleOffsetAt(lastBreakAt);

  const int firstLineIndent = resolveFirstLineIndent(breakIndex == 0 && !firstLineExtracted, renderer, fontId);

  std::vector<std::string> lineRubyTexts(lineWordCount);
  if (!rubyTexts.empty() && lastBreakAt < rubyTexts.size()) {
    const size_t copyCount = std::min(lineBreak, rubyTexts.size()) - lastBreakAt;
    std::copy(rubyTexts.begin() + lastBreakAt, rubyTexts.begin() + lastBreakAt + copyCount, lineRubyTexts.begin());
  }

  const int extraStartOffset = calculateRubyExtraStartOffset(lastBreakAt, lineBreak, renderer, fontId);
  const int extraEndOffset = calculateRubyExtraEndOffset(lastBreakAt, lineBreak, renderer, fontId);

  std::vector<std::string> lineWords;
  lineWords.reserve(lineWordCount);
  std::vector<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.reserve(lineWordCount);

  for (size_t i = 0; i < lineWordCount; ++i) {
    // Copy out of the arena: TextBlock construction and bidi reorder below
    // want owning strings. Bounded by one line; the arena bytes are released
    // by the consumed-prefix pass in layoutAndExtractLines.
    std::string word{wordAt(lastBreakAt + i)};  // brace-init: `word(` is an Arduino macro
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
    lineWords.push_back(std::move(word));
    lineWordStyles.push_back(wordStyles[lastBreakAt + i]);
  }

  // Boundary k (1 <= k < lineWordCount) sits between logical words k-1 and k.
  const auto logicalGap = [&](const size_t k) {
    const size_t boundaryIdx = lastBreakAt + k;
    if (continuesVec[boundaryIdx]) {
      return renderer.getKerning(fontId, lastCodepoint(lineWords[k - 1]), firstCodepoint(lineWords[k]),
                                 lineWordStyles[k - 1], blockStyle.characterSpacing);
    }
    if (noSpaceBeforeVec[boundaryIdx]) {
      return static_cast<int>(blockStyle.characterSpacing);
    }
    return scaleSpace(renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[k - 1]), firstCodepoint(lineWords[k]),
                                               lineWordStyles[k - 1]),
                      wordSpacingPercent);
  };
  const auto logicalStretches = [&](const size_t k) {
    return TokenBoundary::isJustifiableGap(continuesVec[lastBreakAt + k], noSpaceBeforeVec[lastBreakAt + k],
                                           lineWords[k] == " ");
  };
  const auto logicalShrinks = [&](const size_t k) { return logicalStretches(k) && !noSpaceBeforeVec[lastBreakAt + k]; };

  // Calculate total word width for this line, count its justifiable gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  int lineWordWidthSum = 0;
  int stretchGapCount = 0;
  int shrinkGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    if (wordIdx == 0) continue;
    stretchGapCount += logicalStretches(wordIdx);
    shrinkGapCount += logicalShrinks(wordIdx);
    totalNaturalGaps += logicalGap(wordIdx);
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For RTL, implicit/default Left alignment becomes Right alignment.
  // Explicit text-align:left must remain left for CSS correctness.
  const CssTextAlign effectiveAlignment =
      (blockStyle.isRtl && !blockStyle.textAlignDefined && blockStyle.alignment == CssTextAlign::Left)
          ? CssTextAlign::Right
          : blockStyle.alignment;
  const bool justifyLine = effectiveAlignment == CssTextAlign::Justify && !isLastLine;
  // The line breaker lets each space shrink by space / 3 whole pixels.
  const int space = interwordSpace(renderer, fontId);

  // extraEndOffset reserves space for any ruby group at the right edge of the line.
  const int spareSpace = effectivePageWidth - extraStartOffset - extraEndOffset - lineWordWidthSum - totalNaturalGaps;

  // BiDi processing: reorder words with UAX#9 in full-line context.
  visualOrderScratch.clear();
  visualOrderScratch.reserve(lineWordCount);
  // Skip expensive visual-order resolution for pure LTR paragraphs that have no RTL words.
  const bool shouldResolveVisualOrder = blockStyle.isRtl || hasRtlWord;
  const bool willReorder =
      shouldResolveVisualOrder && BidiUtils::computeVisualWordOrder(lineWords, blockStyle.isRtl, visualOrderScratch);

  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  if (willReorder) {
    reorderedWordsScratch.clear();
    reorderedStylesScratch.clear();
    reorderedWidthsScratch.clear();
    reorderedContinuesScratch.clear();
    reorderedNoSpaceBeforeScratch.clear();
    reorderedFocusBoundaryScratch.clear();
    reorderedWordsScratch.reserve(visualOrderScratch.size());
    reorderedStylesScratch.reserve(visualOrderScratch.size());
    reorderedWidthsScratch.reserve(visualOrderScratch.size());
    reorderedContinuesScratch.reserve(visualOrderScratch.size());
    reorderedNoSpaceBeforeScratch.reserve(visualOrderScratch.size());
    reorderedFocusBoundaryScratch.reserve(visualOrderScratch.size());

    for (size_t i = 0; i < visualOrderScratch.size(); ++i) {
      const uint16_t src = visualOrderScratch[i];
      reorderedWordsScratch.push_back(std::move(lineWords[src]));
      reorderedStylesScratch.push_back(lineWordStyles[src]);
      reorderedWidthsScratch.push_back(wordWidths[lastBreakAt + src]);
      reorderedFocusBoundaryScratch.push_back(wordFocusBoundary[lastBreakAt + src]);

      // Continuation means "no break/gap between two adjacent logical tokens".
      // After visual reordering (common in RTL), an adjacent logical pair can appear
      // as either (prev -> curr) or (curr -> prev) in visual order; preserve both.
      bool continues = false;
      if (i > 0) {
        const size_t prevSrc = visualOrderScratch[i - 1];
        const size_t currSrc = src;
        const bool forwardAdjacent = currSrc == prevSrc + 1;
        const bool reverseAdjacent = prevSrc == currSrc + 1;

        if (forwardAdjacent && continuesVec[lastBreakAt + currSrc]) {
          continues = true;
        } else if (reverseAdjacent && continuesVec[lastBreakAt + prevSrc]) {
          continues = true;
        }
      }
      reorderedContinuesScratch.push_back(continues);
      reorderedNoSpaceBeforeScratch.push_back(!continues && noSpaceBeforeVec[lastBreakAt + src]);
    }

    // Boundary k sits between visual words k-1 and k. noSpaceBefore was cleared above wherever continues is set.
    const auto visualGap = [&](const size_t k) {
      if (reorderedNoSpaceBeforeScratch[k]) {
        return static_cast<int>(blockStyle.characterSpacing);
      }
      if (reorderedContinuesScratch[k]) {
        return renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[k - 1]),
                                   firstCodepoint(reorderedWordsScratch[k]), reorderedStylesScratch[k - 1],
                                   blockStyle.characterSpacing);
      }
      return scaleSpace(
          renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[k - 1]),
                                   firstCodepoint(reorderedWordsScratch[k]), reorderedStylesScratch[k - 1]),
          wordSpacingPercent);
    };
    // A CJK/Korean break opportunity (no inserted Latin-style space) still stretches; a continuation only
    // does when it is a literal space token such as a no-break space.
    const auto visualShrinks = [&](const size_t k) {
      return !reorderedNoSpaceBeforeScratch[k] && (!reorderedContinuesScratch[k] || reorderedWordsScratch[k] == " ");
    };
    const auto visualStretches = [&](const size_t k) { return reorderedNoSpaceBeforeScratch[k] || visualShrinks(k); };

    int reorderedWordWidthSum = 0;
    int reorderedStretchGaps = 0;
    int reorderedShrinkGaps = 0;
    int reorderedNaturalGaps = 0;
    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      reorderedWordWidthSum += reorderedWidthsScratch[wordIdx];
      if (wordIdx == 0) continue;
      reorderedStretchGaps += visualStretches(wordIdx);
      reorderedShrinkGaps += visualShrinks(wordIdx);
      reorderedNaturalGaps += visualGap(wordIdx);
    }

    const int reorderedSpare =
        effectivePageWidth - extraStartOffset - extraEndOffset - reorderedWordWidthSum - reorderedNaturalGaps;
    LineJustifier justifier(justifyLine, reorderedSpare, reorderedStretchGaps, reorderedShrinkGaps,
                            reorderedShrinkGaps * (space / 3));
    const int contentWidth = reorderedWordWidthSum + reorderedNaturalGaps + justifier.applied();

    int xpos = 0;
    if (blockStyle.isRtl) {
      if (effectiveAlignment == CssTextAlign::Right || effectiveAlignment == CssTextAlign::Justify) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    } else {
      xpos = firstLineIndent;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    }

    // Gaps are only counted from index 1, so a leading no-break space never takes a justification share (#2185).
    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      if (wordIdx > 0) {
        xpos += reorderedWidthsScratch[wordIdx - 1] + visualGap(wordIdx) +
                justifier.nextGap(visualStretches(wordIdx), visualShrinks(wordIdx));
      }
      lineXPos.push_back(static_cast<int16_t>(xpos));
    }

    lineWords.swap(reorderedWordsScratch);
    lineWordStyles.swap(reorderedStylesScratch);
  } else {
    LineJustifier justifier(justifyLine, spareSpace, stretchGapCount, shrinkGapCount, shrinkGapCount * (space / 3));
    // Standard LTR/RTL positioning loop when no visual reordering is needed
    if (blockStyle.isRtl) {
      // RTL: position words from right to left
      int xpos = effectivePageWidth;
      if (effectiveAlignment == CssTextAlign::Left) {
        // Explicit left alignment in RTL context
        xpos = lineWordWidthSum + totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth + lineWordWidthSum + totalNaturalGaps) / 2;
      }
      // For Right and Justify, start from right edge (xpos = effectivePageWidth)

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        xpos -= wordWidths[lastBreakAt + wordIdx];
        lineXPos.push_back(static_cast<int16_t>(xpos));
        if (wordIdx + 1 < lineWordCount) {
          xpos -=
              logicalGap(wordIdx + 1) + justifier.nextGap(logicalStretches(wordIdx + 1), logicalShrinks(wordIdx + 1));
        }
      }
    } else {
      // LTR: position words from left to right
      int xpos = firstLineIndent + extraStartOffset;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
      }

      // Gaps are only counted from index 1, so a leading no-break space never takes a justification share (#2185).
      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        if (wordIdx > 0) {
          xpos += wordWidths[lastBreakAt + wordIdx - 1] + logicalGap(wordIdx) +
                  justifier.nextGap(logicalStretches(wordIdx), logicalShrinks(wordIdx));
        }
        lineXPos.push_back(static_cast<int16_t>(xpos));
      }
    }
  }

  const auto focusBoundaryAt = [&](const size_t idx) {
    return willReorder ? reorderedFocusBoundaryScratch[idx] : wordFocusBoundary[lastBreakAt + idx];
  };

  std::vector<TextBlock::LinkSpan> lineLinks;
  std::vector<uint8_t> lineLinkIdsSeen;
  for (size_t i = 0; i < lineWordCount; i++) {
    const uint8_t linkId = wordLinkIds[lastBreakAt + (willReorder ? visualOrderScratch[i] : i)];
    if (linkId == 0 || linkId > linkTargets.size()) continue;

    size_t spanIndex = 0;
    while (spanIndex < lineLinkIdsSeen.size() && lineLinkIdsSeen[spanIndex] != linkId) spanIndex++;
    int width = willReorder ? reorderedWidthsScratch[i] : wordWidths[lastBreakAt + i];
    const int right = lineXPos[i] + width;
    const int topLift =
        (lineWordStyles[i] & EpdFontFamily::SUP) != 0 ? renderer.getFontAscenderSize(fontId) * 2 / 5 : 0;

    if (spanIndex == lineLinkIdsSeen.size()) {
      lineLinks.emplace_back();
      auto& span = lineLinks.back();
      strncpy(span.href, linkTargets[linkId - 1].c_str(), sizeof(span.href) - 1);
      span.href[sizeof(span.href) - 1] = '\0';
      span.x = lineXPos[i];
      span.width = static_cast<int16_t>(width);
      span.topLift = static_cast<int16_t>(topLift);
      lineLinkIdsSeen.push_back(linkId);
    } else {
      auto& span = lineLinks[spanIndex];
      const int left = std::min<int>(span.x, lineXPos[i]);
      const int mergedRight = std::max<int>(span.x + span.width, right);
      span.x = static_cast<int16_t>(left);
      span.width = static_cast<int16_t>(mergedRight - left);
      span.topLift = std::max<int16_t>(span.topLift, static_cast<int16_t>(topLift));
    }
  }

  // Fast path: no word on this line carries focus emphasis, so pass empty boundary/suffixX
  // vectors. TextBlock pays zero per-word RAM cost for these annotations when they are empty.
  bool lineHasFocusSplit = false;
  for (size_t i = 0; i < lineWordCount; i++) {
    if (focusBoundaryAt(i) != 0) {
      lineHasFocusSplit = true;
      break;
    }
  }

  if (!lineHasFocusSplit) {
    // TextBlock flattens the vectors into its arena; they stay owned here and die at return.
    auto block = makeUniqueNoThrow<TextBlock>(lineWords, lineXPos, lineWordStyles, std::vector<uint8_t>{},
                                              std::vector<uint16_t>{}, blockStyle, std::move(lineRubyTexts),
                                              std::move(lineLinks));
    if (!block || !block->valid()) {
      LOG_ERR("PTX", "Dropping line: TextBlock or arena allocation failed");
      // Latch through the same flag as addWord() OOM: the caller releases the
      // consumed words right after this returns, so without it the section
      // would commit with this line silently missing.
      droppedWords = true;
      return;
    }
    processLine(std::move(block), lineVisibleOffset);
    return;
  }

  // Each word is one TextBlock entry carrying its own boundary; all that remains is the suffix x
  // offset the renderer needs to resume in regular weight, i.e. the bold prefix's advance.
  std::vector<uint8_t> outBoundaries;
  std::vector<uint16_t> outSuffixX;
  outBoundaries.reserve(lineWordCount);
  outSuffixX.reserve(lineWordCount);
  for (size_t i = 0; i < lineWordCount; i++) {
    const uint8_t boundary = focusBoundaryAt(i);
    outBoundaries.push_back(boundary);
    outSuffixX.push_back(boundary == 0 ? 0
                                       : measureFocusPrefixAdvance(renderer, fontId, lineWords[i], lineWordStyles[i],
                                                                   boundary, blockStyle.characterSpacing));
  }

  auto block = makeUniqueNoThrow<TextBlock>(lineWords, lineXPos, lineWordStyles, outBoundaries, outSuffixX, blockStyle,
                                            std::move(lineRubyTexts), std::move(lineLinks));
  if (!block || !block->valid()) {
    LOG_ERR("PTX", "Dropping line: TextBlock or arena allocation failed");
    droppedWords = true;  // see the non-focus branch above
    return;
  }
  processLine(std::move(block), lineVisibleOffset);
}

#include "core/lang/AnswerCheck.h"

#include <cstring>

#include "core/lang/Utf8.h"

namespace tinta::core::lang {
namespace {

// Latin-1 letters U+00C0..U+00FF folded to ASCII (lower case); "" for the
// two signs in that block. Two-letter results are ae, th and ss.
const char* const kFolded[64] = {
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i",  "i",  //
    "d", "n", "o", "o", "o", "o", "o",  "",  "o", "u", "u", "u", "u", "y", "th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i",  "i",  //
    "d", "n", "o", "o", "o", "o", "o",  "",  "o", "u", "u", "u", "u", "y", "th", "y",
};

bool latinLetter(uint32_t cp) { return cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7; }

// Shared walk: letters and digits kept (lower case, or folded), anything else
// dropped, whitespace collapsed to single spaces between words.
size_t clean(const char* text, size_t length, char* out, size_t cap, bool fold) {
  if (cap == 0) return 0;
  size_t used = 0;
  bool pendingSpace = false;
  const auto put = [&](const char* bytes, size_t n) {
    if (pendingSpace && used > 0 && used + 1 < cap) out[used++] = ' ';
    pendingSpace = false;
    for (size_t i = 0; i < n && used + 1 < cap; ++i) out[used++] = bytes[i];
  };
  for (size_t i = 0; text && i < length && text[i];) {
    uint32_t cp = 0;
    i += utf8::decode(text + i, length - i, cp);
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == 0xA0) {
      pendingSpace = true;
    } else if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
      const char c = static_cast<char>(cp);
      put(&c, 1);
    } else if (cp >= 'A' && cp <= 'Z') {
      const char c = static_cast<char>(cp + 32);
      put(&c, 1);
    } else if (latinLetter(cp)) {
      if (fold) {
        const char* f = kFolded[cp - 0xC0];
        put(f, std::strlen(f));
      } else {
        // Capitals U+00C0..U+00DE sit 0x20 below their small letters (ß and
        // ÿ have no capital here).
        if (cp >= 0xC0 && cp <= 0xDE) cp += 0x20;
        char bytes[4];
        put(bytes, utf8::encode(cp, bytes));
      }
    }
  }
  out[used] = '\0';
  return used;
}

// Calls visit(text, length) for each "; "-separated part of `list`.
template <class Visit>
bool anyPart(const char* list, Visit&& visit) {
  if (!list) return false;
  const char* start = list;
  for (const char* p = list;; ++p) {
    if (*p == ';' || *p == '\0') {
      if (p > start && visit(start, static_cast<size_t>(p - start))) return true;
      if (*p == '\0') return false;
      start = p + 1;
    }
  }
}

}  // namespace

size_t normalizeAnswer(const char* text, size_t length, char* out, size_t cap) {
  return clean(text, length, out, cap, false);
}

size_t foldAnswer(const char* text, size_t length, char* out, size_t cap) {
  return clean(text, length, out, cap, true);
}

uint8_t editDistance(const char* a, size_t aLength, const char* b, size_t bLength, uint8_t limit) {
  if (aLength > kMaxAnswerBytes) aLength = kMaxAnswerBytes;
  if (bLength > kMaxAnswerBytes) bLength = kMaxAnswerBytes;
  const size_t gap = aLength > bLength ? aLength - bLength : bLength - aLength;
  if (gap > limit) return static_cast<uint8_t>(limit + 1);
  uint8_t prev[kMaxAnswerBytes + 1];
  uint8_t row[kMaxAnswerBytes + 1];
  for (size_t j = 0; j <= bLength; ++j) prev[j] = static_cast<uint8_t>(j > 255 ? 255 : j);
  for (size_t i = 1; i <= aLength; ++i) {
    row[0] = static_cast<uint8_t>(i > 255 ? 255 : i);
    uint8_t best = row[0];
    for (size_t j = 1; j <= bLength; ++j) {
      const uint8_t substitute = static_cast<uint8_t>(prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1));
      const uint8_t remove = static_cast<uint8_t>(prev[j] + 1);
      const uint8_t insert = static_cast<uint8_t>(row[j - 1] + 1);
      uint8_t v = substitute < remove ? substitute : remove;
      v = v < insert ? v : insert;
      row[j] = v;
      if (v < best) best = v;
    }
    if (best > limit) return static_cast<uint8_t>(limit + 1);
    std::memcpy(prev, row, bLength + 1);
  }
  return prev[bLength] > limit ? static_cast<uint8_t>(limit + 1) : prev[bLength];
}

Verdict checkAnswer(const char* typed, const char* expected, const char* alternatives) {
  char given[kMaxAnswerBytes + 1];
  char givenFolded[kMaxAnswerBytes + 1];
  const size_t givenLength = normalizeAnswer(typed, typed ? std::strlen(typed) : 0, given, sizeof given);
  const size_t foldedLength = foldAnswer(typed, typed ? std::strlen(typed) : 0, givenFolded, sizeof givenFolded);
  if (givenLength == 0 || !expected) return Verdict::Wrong;

  // Each check walks the answers once; the first that holds decides, best
  // verdict first.
  const auto exact = [&](const char* text, size_t length) {
    char want[kMaxAnswerBytes + 1];
    const size_t n = normalizeAnswer(text, length, want, sizeof want);
    return n == givenLength && std::memcmp(want, given, n) == 0;
  };
  const auto folded = [&](const char* text, size_t length) {
    char want[kMaxAnswerBytes + 1];
    const size_t n = foldAnswer(text, length, want, sizeof want);
    return n > 0 && n == foldedLength && std::memcmp(want, givenFolded, n) == 0;
  };
  const auto typo = [&](const char* text, size_t length) {
    char want[kMaxAnswerBytes + 1];
    const size_t n = foldAnswer(text, length, want, sizeof want);
    size_t letters = 0;
    for (size_t i = 0; i < n; ++i) letters += want[i] != ' ' ? 1 : 0;
    return letters >= 6 && editDistance(want, n, givenFolded, foldedLength, 1) <= 1;
  };

  if (anyPart(expected, exact)) return Verdict::Exact;
  if (anyPart(alternatives, exact) || anyPart(alternatives, folded)) return Verdict::Alternative;
  if (anyPart(expected, folded)) return Verdict::Accents;
  if (anyPart(expected, typo)) return Verdict::Typo;
  return Verdict::Wrong;
}

}  // namespace tinta::core::lang

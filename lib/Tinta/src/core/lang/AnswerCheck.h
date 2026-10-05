#pragma once

#include <cstddef>
#include <cstdint>

namespace tinta::core::lang {

// Checking a typed answer (PLAN.md 8.4). Both sides are normalised first:
// lower case (Latin-1 capitals too), punctuation dropped (so `¿ ¡` and a
// final full stop never count), runs of spaces collapsed, ends trimmed.
// Accents are kept for the exact comparison and folded away for the lenient
// ones.
enum class Verdict : uint8_t {
  Wrong,        // none of the below
  Exact,        // the expected answer
  Accents,      // only the accents differ (esta for está; ñ counts as an accent)
  Typo,         // one edit away, on an answer of six or more letters
  Alternative,  // an accepted non-Mexican synonym; show the Mexican word
};

// The longest answer compared, in bytes after normalising; longer input is
// cut, which only ever makes a match less likely.
inline constexpr size_t kMaxAnswerBytes = 96;

// `alternatives` is the lemma's `alt` list ("ordenador; computador"), may be
// null. `expected` may itself list several right answers separated by "; ".
Verdict checkAnswer(const char* typed, const char* expected, const char* alternatives = nullptr);

// Exact and Alternative are right; Accents and Typo are right but shaky.
inline bool accepted(Verdict v) { return v != Verdict::Wrong; }
inline bool shaky(Verdict v) { return v == Verdict::Accents || v == Verdict::Typo; }

// Normalised form, for display and tests: writes at most cap - 1 bytes and a
// NUL; returns the length written.
size_t normalizeAnswer(const char* text, size_t length, char* out, size_t cap);
// The same with accents folded (á -> a, ñ -> n, ü -> u).
size_t foldAnswer(const char* text, size_t length, char* out, size_t cap);

// Levenshtein distance between two byte strings of at most kMaxAnswerBytes,
// stopping once it exceeds `limit` (then returns limit + 1).
uint8_t editDistance(const char* a, size_t aLength, const char* b, size_t bLength, uint8_t limit);

}  // namespace tinta::core::lang

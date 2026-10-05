#pragma once

#include <cstddef>

namespace tinta::core {

// Search-key folding, docs/pack-format.md section 2: the device's copy of
// tools/packc/fold.py. Keys in the pack were made by the Python function, so
// the two must agree byte for byte (test/host/pack_test checks the vectors of
// `fold.py --vectors`).
//
// Writes the key of the NUL-terminated UTF-8 string `utf8` (nullptr reads as
// "") to out and returns the key's full length, like snprintf. A result
// >= outSize means the key was cut: out then holds its first outSize - 1
// bytes, i.e. exactly Python's fold(text)[:outSize - 1]. That prefix may end
// inside a two-letter output (the "s" of "ss" for ß, "a" of "ae", "t" of "th")
// or on a separator space, so callers that need whole keys must treat a cut
// key as "no match" rather than search with it. out is always NUL-terminated
// when outSize > 0; out may be nullptr when outSize is 0.
//
// Two differences from Python, neither reachable with text in the pack
// charset (which is already NFC):
//  - No NFC step. A decomposed letter (e + U+0301) folds to its base letter,
//    which is what NFC + fold gives for every Latin-1 letter; but where NFC
//    would compose to a codepoint above U+00FF (a + U+0304 -> U+0101) Python
//    drops the letter and this keeps it, and compatibility singletons such as
//    U+212A KELVIN SIGN (NFC: K) are dropped here.
//  - Malformed UTF-8 (stray continuation, overlong, surrogate, truncated
//    sequence) is dropped one byte at a time; Python never sees such input.
size_t foldKey(const char* utf8, char* out, size_t outSize);

}  // namespace tinta::core

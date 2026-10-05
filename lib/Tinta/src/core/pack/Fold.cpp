#include "core/pack/Fold.h"

#include <cstdint>
#include <cstring>

#include "core/lang/Utf8.h"

namespace tinta::core {

namespace {

// Folds of U+00C0..U+00FF; "" drops the codepoint (the signs × and ÷).
// U+00A0..U+00BF (punctuation, ª, º, µ, ...) are all dropped.
constexpr char kLatin1[64][3] = {
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i",  "i",   // C0
    "d", "n", "o", "o", "o", "o", "o",  "",  "o", "u", "u", "u", "u", "y", "th", "ss",  // D0
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i",  "i",   // E0
    "d", "n", "o", "o", "o", "o", "o",  "",  "o", "u", "u", "u", "u", "y", "th", "y",   // F0
};

}  // namespace

size_t foldKey(const char* utf8, char* out, size_t outSize) {
  const char* s = utf8 != nullptr ? utf8 : "";
  const size_t len = std::strlen(s);
  size_t n = 0;  // full key length; bytes past outSize - 1 are counted, not stored
  bool pendingSpace = false;
  const auto put = [&](char c) {
    if (n + 1 < outSize) out[n] = c;
    ++n;
  };

  for (size_t i = 0; i < len;) {
    uint32_t cp = 0;
    i += utf8::decode(s + i, len - i, cp);
    char ascii[2] = {0, 0};
    const char* piece = ascii;
    if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
      ascii[0] = static_cast<char>(cp);
    } else if (cp >= 'A' && cp <= 'Z') {
      ascii[0] = static_cast<char>(cp - 'A' + 'a');
    } else if (cp >= 0xC0 && cp <= 0xFF) {
      piece = kLatin1[cp - 0xC0];
    } else if (cp == ' ' || cp == '\t' || cp == '\n') {
      // A separator only counts between two kept pieces, so leading and
      // trailing ones vanish and runs collapse to one space.
      pendingSpace = n > 0;
      continue;
    }
    if (*piece == '\0') continue;  // dropped; a pending space stays pending
    if (pendingSpace) {
      put(' ');
      pendingSpace = false;
    }
    while (*piece != '\0') put(*piece++);
  }

  if (outSize > 0) out[n < outSize ? n : outSize - 1] = '\0';
  return n;
}

}  // namespace tinta::core

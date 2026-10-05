// modules: lang

#include "core/lang/Utf8.h"

#include <cstdint>
#include <cstring>

#include "check.h"

using namespace tinta::core;

namespace {

uint32_t decodeOne(const char* s, size_t len, size_t& used) {
  uint32_t cp = 0;
  used = utf8::decode(s, len, cp);
  return cp;
}

void testDecodeValid() {
  size_t n = 0;
  CHECK_EQ(decodeOne("A", 1, n), 'A');
  CHECK_EQ(n, 1);
  CHECK_EQ(decodeOne("\xC3\x91", 2, n), 0xD1);  // Ñ
  CHECK_EQ(n, 2);
  CHECK_EQ(decodeOne("\xC2\xBF", 2, n), 0xBF);        // ¿
  CHECK_EQ(decodeOne("\xE2\x80\x94", 3, n), 0x2014);  // —
  CHECK_EQ(n, 3);
  CHECK_EQ(decodeOne("\xF0\x9F\x99\x82", 4, n), 0x1F642);
  CHECK_EQ(n, 4);
  CHECK_EQ(decodeOne("\xEF\xBF\xBD", 3, n), utf8::kReplacement);  // a real U+FFFD
  CHECK_EQ(n, 3);
  CHECK_EQ(decodeOne("", 0, n), utf8::kReplacement);
  CHECK_EQ(n, 0);
}

void testDecodeMalformedConsumesOneByte() {
  const char* cases[] = {
      "\x80",              // stray continuation
      "\xC0\x80",          // overlong NUL
      "\xE0\x80\xAF",      // overlong '/'
      "\xED\xA0\x80",      // surrogate
      "\xF4\x90\x80\x80",  // above U+10FFFF
      "\xE2\x80",          // truncated
      ("\xC3"
       "A"),  // lead byte without continuation
      "\xFF",
  };
  for (const char* s : cases) {
    size_t n = 0;
    CHECK_EQ(decodeOne(s, std::strlen(s), n), utf8::kReplacement);
    CHECK_EQ(n, 1);
  }
}

void testEncodeRoundTrip() {
  for (uint32_t cp = 0; cp <= 0x10FFFF; cp += (cp < 0x3000 ? 1 : 0x101)) {
    if (cp >= 0xD800 && cp <= 0xDFFF) continue;
    char buf[4];
    const size_t len = utf8::encode(cp, buf);
    CHECK_EQ(len, utf8::encodedLength(cp));
    size_t used = 0;
    const uint32_t back = decodeOne(buf, len, used);
    if (back != cp || used != len) {
      CHECK_EQ(back, cp);
      return;
    }
  }
  char buf[4];
  CHECK_EQ(utf8::encode(0xD800, buf), 3);  // surrogate -> U+FFFD
  CHECK(std::memcmp(buf, "\xEF\xBF\xBD", 3) == 0);
  CHECK_EQ(utf8::encode(0x110000, buf), 3);
  CHECK_EQ(utf8::encodedLength(0x110000), 3);
}

void testNextAndPrevious() {
  // "¿Él?" then a stray continuation byte, then "—".
  const char s[] = "\xC2\xBF\xC3\x89l?\x80\xE2\x80\x94";
  const size_t len = sizeof(s) - 1;
  const size_t bounds[] = {0, 2, 4, 5, 6, 7, 10};
  size_t i = 0;
  for (size_t k = 1; k < sizeof(bounds) / sizeof(bounds[0]); ++k) {
    i = utf8::next(s, len, i);
    CHECK_EQ(i, bounds[k]);
  }
  CHECK_EQ(utf8::next(s, len, len), len);
  for (size_t k = sizeof(bounds) / sizeof(bounds[0]) - 1; k > 0; --k) {
    CHECK_EQ(utf8::previous(s, bounds[k]), bounds[k - 1]);
  }
  CHECK_EQ(utf8::previous(s, 0), 0);
  CHECK_EQ(utf8::count(s, len), 6);

  // Four continuation bytes after a lead: each extra one is its own codepoint.
  const char odd[] = "\xC3\x89\x89\x89";
  CHECK_EQ(utf8::previous(odd, 4), 3);
  CHECK_EQ(utf8::previous(odd, 2), 0);
  CHECK_EQ(utf8::count(odd, 4), 3);
}

void testConstexpr() {
  constexpr uint32_t cp = [] {
    uint32_t v = 0;
    utf8::decode("\xC3\xB1", 2, v);
    return v;
  }();
  static_assert(cp == 0xF1, "decode is constexpr");
  static_assert(utf8::count("a\xC3\xB1"
                            "b",
                            4) == 3,
                "count is constexpr");
}

}  // namespace

int main() {
  testDecodeValid();
  testDecodeMalformedConsumesOneByte();
  testEncodeRoundTrip();
  testNextAndPrevious();
  testConstexpr();
  return tinta_test::result();
}

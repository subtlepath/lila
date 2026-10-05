// modules: lang

#include "core/lang/Charset.h"

#include <cstdint>
#include <cstring>

#include "check.h"
#include "fixtures/charset_expected.h"

using namespace tinta::core;

namespace {

int32_t expectedSlot(uint32_t cp, bool& supported) {
  for (const ExpectedSlot& e : kExpectedSupported) {
    if (e.cp == cp) {
      supported = true;
      return e.slot;
    }
  }
  supported = false;
  return -1;
}

void testRangeMatchesPython() {
  CHECK_EQ(charset::kFirst, kExpectedFirst);
  CHECK_EQ(charset::kLast, kExpectedLast);
  CHECK_EQ(sizeof(kExpectedSlotSource) / sizeof(kExpectedSlotSource[0]), charset::kLast - charset::kFirst + 1);
}

void testSlotSourceMatchesPython() {
  for (uint32_t slot = charset::kFirst; slot <= charset::kLast; ++slot) {
    CHECK_EQ(charset::slotSource(slot), kExpectedSlotSource[slot - charset::kFirst]);
  }
  CHECK_EQ(charset::slotSource(0x1F), 0);
  CHECK_EQ(charset::slotSource(0x100), 0);
}

void testSlotForMatchesPythonOnTheBmp() {
  for (uint32_t cp = 0; cp < 0x10000; ++cp) {
    bool supported = false;
    const int32_t slot = expectedSlot(cp, supported);
    if (charset::isSupported(cp) != supported) {
      CHECK_EQ(cp, 0xFFFFFFFF);  // prints the codepoint that disagrees
      return;
    }
    CHECK_EQ(charset::slotFor(cp), slot);
  }
  CHECK(!charset::isSupported(0x1F642));
  CHECK_EQ(charset::slotFor(0x10FFFF), -1);
}

void testRoundTrip() {
  for (const charset::Remap& r : charset::kRemap) {
    CHECK_EQ(charset::slotSource(static_cast<uint32_t>(charset::slotFor(r.unicode))), r.unicode);
  }
  static_assert(charset::slotFor(0x2014) == 0x97, "em dash slot");
  static_assert(charset::slotSource(0x97) == 0x2014, "em dash source");
  static_assert(charset::isSupported(0x2026) && charset::slotFor(0x2026) == -1, "ellipsis");
}

void testLiteralSpellings() {
  struct Spelling {
    const char* literal;
    uint32_t unicode;
  };
  const Spelling spellings[] = {
      {TINTA_EURO, 0x20AC},  {TINTA_LSQUO, 0x2018},  {TINTA_RSQUO, 0x2019},   {TINTA_LDQUO, 0x201C},
      {TINTA_RDQUO, 0x201D}, {TINTA_BULLET, 0x2022}, {TINTA_EN_DASH, 0x2013}, {TINTA_EM_DASH, 0x2014},
  };
  CHECK_EQ(sizeof(spellings) / sizeof(spellings[0]), sizeof(charset::kRemap) / sizeof(charset::kRemap[0]));
  for (const Spelling& s : spellings) {
    uint32_t cp = 0;
    CHECK_EQ(utf8::decode(s.literal, std::strlen(s.literal), cp), std::strlen(s.literal));
    CHECK_EQ(cp, static_cast<uint32_t>(charset::slotFor(s.unicode)));
  }
}

void testEncodeMatchesPython() {
  for (const ExpectedEncoding& e : kExpectedEncodings) {
    char out[128];
    const int32_t n = charset::encode(e.in, std::strlen(e.in), out, sizeof(out));
    if (e.out == nullptr) {
      CHECK_EQ(n, -1);
      continue;
    }
    CHECK(n >= 0);
    if (n < 0) continue;
    out[n] = '\0';
    CHECK_STR_EQ(out, e.out);
  }
}

void testEncodeInPlaceAndLimits() {
  char text[] =
      "\xE2\x80\x94S\xC3\xAD, \xE2\x80\x9C"
      "a\xE2\x80\x9D";  // —Sí, “a”
  const int32_t n = charset::encode(text, std::strlen(text), text, sizeof(text));
  CHECK_EQ(n, 12);
  CHECK(std::memcmp(text,
                    "\xC2\x97S\xC3\xAD, \xC2\x93"
                    "a\xC2\x94",
                    12) == 0);

  char small[4];
  CHECK_EQ(charset::encode("hola", 4, small, sizeof(small)), 4);
  CHECK_EQ(charset::encode("hola!", 5, small, sizeof(small)), -1);
  CHECK_EQ(charset::encode("\xC3", 1, small, sizeof(small)), -1);      // truncated
  CHECK_EQ(charset::encode("\xC2\x97", 2, small, sizeof(small)), -1);  // already a slot
}

}  // namespace

int main() {
  testRangeMatchesPython();
  testSlotSourceMatchesPython();
  testSlotForMatchesPythonOnTheBmp();
  testRoundTrip();
  testLiteralSpellings();
  testEncodeMatchesPython();
  testEncodeInPlaceAndLimits();
  return tinta_test::result();
}

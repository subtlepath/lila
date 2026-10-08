// modules: pack search session lang srs text
//
// The pack read from a file through CachedSource and the string arena, as the
// SD card reads it on the device, against the same pack in memory: every
// string, the records and verb forms, the key searches and nextLetters. Then
// the arena's rules: strings live until the next pass, overflow reads as "",
// and a kept string is visibly stale under poisoning. Uses fixtures/full.pack
// when present, else the simulator fixture.

#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "core/pack/Pack.h"
#include "core/search/Search.h"
#include "core/session/Exercise.h"
#include "host_pack.h"

namespace {

namespace pk = tinta::core::pack;
namespace sr = tinta::core::search;
using tinta_test::HostFileSource;

const char* gPath = "fixtures/full.pack";
std::vector<uint8_t> gImage;
pk::Pack gMem;
HostFileSource gFile;
char gArena[6 * 1024];
pk::Pack gSd;

bool loadImage(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  uint8_t chunk[4096];
  size_t n = 0;
  gImage.clear();
  while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) gImage.insert(gImage.end(), chunk, chunk + n);
  std::fclose(f);
  return true;
}

void testStrings() {
  const uint32_t count = gMem.count(pk::Section::Strs);
  uint32_t offset = 1;
  uint32_t checked = 0;
  for (uint32_t i = 1; i < count; ++i) {
    const char* expect = gMem.str(offset);
    gSd.beginPass();
    const char* got = gSd.str(offset);
    if (std::strcmp(expect, got) != 0) {
      std::printf("  string at %u differs\n", offset);
      CHECK(false);
      break;
    }
    char copy[48];
    const uint32_t n = gSd.copyStr(offset, copy, sizeof copy);
    CHECK_EQ(n, std::strlen(expect) < sizeof copy - 1 ? std::strlen(expect) : sizeof copy - 1);
    CHECK(std::strncmp(copy, expect, n) == 0);
    uint32_t h = 2166136261u;
    for (const char* p = expect; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
    CHECK_EQ(gSd.hashStr(offset), h);
    struct Visitor {
      const char* expected;
      uint32_t bytes;
    } visitor{expect, 0};
    const auto visit = [](void* context, const uint8_t* bytes, uint32_t length) {
      auto& state = *static_cast<Visitor*>(context);
      if (std::memcmp(state.expected + state.bytes, bytes, length) != 0) return false;
      state.bytes += length;
      return true;
    };
    CHECK(gSd.visitStr(offset, &visitor, visit));
    CHECK_EQ(visitor.bytes, std::strlen(expect));
    const auto reject = [](void*, const uint8_t*, uint32_t) { return false; };
    if (*expect) CHECK(!gSd.visitStr(offset, nullptr, reject));
    offset += static_cast<uint32_t>(std::strlen(expect)) + 1;
    ++checked;
  }
  std::printf("  %u strings, arena peak %u bytes, %u overflows\n", checked, gSd.arenaPeak(), gSd.arenaOverflows());
  CHECK_EQ(gSd.arenaOverflows(), 0u);
  // Out of range and offset 0.
  CHECK_STR_EQ(gSd.str(0), "");
  CHECK_STR_EQ(gSd.str(0xFFFFFFF0u), "");
  CHECK(!gSd.visitStr(0, nullptr, nullptr));
}

void testRecords() {
  for (uint32_t i = 0; i < gMem.itemCount(); ++i) {
    pk::Item a;
    pk::Item b;
    CHECK(gMem.item(i, a) && gSd.item(i, b));
    CHECK(std::memcmp(&a, &b, sizeof a) == 0);
    CHECK_EQ(gSd.uidAt(i), gMem.uidAt(i));
    CHECK_EQ(gSd.indexOfUid(gMem.uidAt(i)), gMem.indexOfUid(gMem.uidAt(i)));
  }
  // Uids the pack does not have, below, inside and above its range.
  for (const uint32_t uid : {0u, 1u, 2u, 3000u, gMem.itemCount() + 1u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
    CHECK_EQ(gSd.indexOfUid(uid), gMem.indexOfUid(uid));
  }
  for (uint32_t t = 0; t < gMem.count(pk::Section::Verb); ++t) {
    for (uint8_t tense = 0; tense < pk::kTenseCount; ++tense) {
      for (uint8_t person = 0; person < pk::kPersonCount; ++person) {
        gSd.beginPass();
        CHECK_STR_EQ(
            gSd.verbForm(static_cast<uint16_t>(t), static_cast<pk::Tense>(tense), static_cast<pk::Person>(person)),
            gMem.verbForm(static_cast<uint16_t>(t), static_cast<pk::Tense>(tense), static_cast<pk::Person>(person)));
      }
    }
  }
  for (uint32_t s = 0; s < gMem.count(pk::Section::Sent); ++s) {
    pk::Sentence sentence;
    gMem.sentence(static_cast<uint16_t>(s), sentence);
    gSd.beginPass();
    for (uint8_t i = 0; i < sentence.tokenCount; ++i) {
      pk::Token token;
      gMem.token(sentence, i, token);
      const pk::TextSpan a = gMem.tokenText(sentence, token);
      const pk::TextSpan b = gSd.tokenText(sentence, token);
      CHECK(a.length == b.length && std::memcmp(a.text, b.text, a.length) == 0);
    }
  }
}

void testSearches() {
  uint32_t searches = 0;
  for (uint32_t i = 0; i < gMem.count(pk::Section::Lkey); i += 7) {
    pk::LemmaKey key;
    gMem.lemmaKeyAt(i, key);
    const std::string word = gMem.str(key.key);
    gSd.beginPass();
    for (const bool prefix : {false, true}) {
      const pk::KeyRange a = prefix ? gMem.findLemmaPrefix(word.c_str()) : gMem.findLemma(word.c_str());
      const pk::KeyRange b = prefix ? gSd.findLemmaPrefix(word.c_str()) : gSd.findLemma(word.c_str());
      CHECK(a.first == b.first && a.count == b.count);
    }
    const pk::KeyRange fa = gMem.findForm(word.c_str());
    const pk::KeyRange fb = gSd.findForm(word.c_str());
    CHECK(fa.first == fb.first && fa.count == fb.count);
    ++searches;
  }
  for (uint32_t i = 0; i < gMem.count(pk::Section::Ekey); i += 11) {
    pk::EnglishKey key;
    gMem.englishAt(i, key);
    const std::string word = gMem.str(key.key);
    gSd.beginPass();
    const pk::KeyRange a = gMem.findEnglishPrefix(word.c_str());
    const pk::KeyRange b = gSd.findEnglishPrefix(word.c_str());
    CHECK(a.first == b.first && a.count == b.count);
    ++searches;
  }

  // nextLetters searches inside the prefix's range: the same letters as a
  // search of the whole table for each.
  char prefix[4] = {};
  uint32_t prefixes = 0;
  for (int a = -1; a < 26; ++a) {
    for (int b = -1; b < (a < 0 ? 0 : 26); ++b) {
      size_t n = 0;
      if (a >= 0) prefix[n++] = static_cast<char>('a' + a);
      if (b >= 0) prefix[n++] = static_cast<char>('a' + b);
      prefix[n] = '\0';
      for (const bool english : {false, true}) {
        uint32_t expect = 0;
        char probe[8];
        for (uint8_t i = 0; i < 26; ++i) {
          std::snprintf(probe, sizeof probe, "%s%c", prefix, 'a' + i);
          const pk::KeyRange r = english ? gMem.findEnglishPrefix(probe) : gMem.findLemmaPrefix(probe);
          if (r.count > 0) expect |= 1u << i;
        }
        gSd.beginPass();
        const uint32_t got = sr::nextLetters(gSd, prefix, english) & ((1u << 26) - 1);
        CHECK_EQ(got, expect);
        CHECK_EQ(sr::nextLetters(gSd, prefix, english), sr::nextLetters(gMem, prefix, english));
      }
      ++prefixes;
    }
  }
  std::printf("  %u key searches, %u nextLetters prefixes\n", searches, prefixes);
}

void testArena() {
  // A string lives until the next pass; the same offset twice is one copy.
  pk::Lemma lemma;
  gMem.lemma(0, lemma);
  gSd.beginPass();
  const char* first = gSd.str(lemma.es);
  CHECK(first == gSd.str(lemma.es));
  const std::string saved = first;

  // Poisoned, a string kept across a pass no longer reads as itself.
  gSd.setPoison(true);
  gSd.beginPass();
  CHECK(saved != first);
  gSd.setPoison(false);

  // Overflow: "" and counted.
  HostFileSource file;
  CHECK(file.open(gPath, 4));
  char tiny[8];
  pk::Pack small;
  small.setArena(tiny, sizeof tiny);
  CHECK(small.open(file) == pk::PackStatus::Ok);
  uint32_t longOffset = 0;
  for (uint16_t i = 0; i < gMem.count(pk::Section::Lemm) && longOffset == 0; ++i) {
    pk::Lemma l;
    gMem.lemma(i, l);
    if (std::strlen(gMem.str(l.en)) >= sizeof tiny) longOffset = l.en;
  }
  CHECK(longOffset != 0);
  CHECK_STR_EQ(small.str(longOffset), "");
  CHECK_EQ(small.arenaOverflows(), 1u);
  // No arena at all: every string from a file reads as "".
  pk::Pack none;
  CHECK(none.open(file) == pk::PackStatus::Ok);
  CHECK_STR_EQ(none.str(longOffset), "");

  // The CRC streams through the source.
  CHECK(gSd.verifyCrc());
  CHECK_EQ(gSd.headerCrc(), gMem.headerCrc());
}

// Word-order tiles keep their own copy of the sentence, so they survive the
// passes between being dealt and being drawn.
void testWordOrderSurvivesPasses() {
  gSd.setPoison(true);
  uint32_t dealt = 0;
  for (uint32_t s = 0; s < gMem.count(pk::Section::Sent) && dealt < 200; ++s) {
    pk::Sentence sentence;
    gMem.sentence(static_cast<uint16_t>(s), sentence);
    tinta::core::session::WordOrder fromMem;
    tinta::core::session::WordOrder fromSd;
    gSd.beginPass();
    if (!fromMem.begin(gMem, sentence, s + 1)) continue;
    CHECK(fromSd.begin(gSd, sentence, s + 1));
    gSd.beginPass();
    gSd.str(sentence.en);  // reuse the arena
    for (uint8_t i = 0; i < fromMem.tileCount(); ++i) {
      const pk::TextSpan a = fromMem.tile(i);
      const pk::TextSpan b = fromSd.tile(i);
      CHECK(a.length == b.length && std::memcmp(a.text, b.text, a.length) == 0);
    }
    ++dealt;
  }
  gSd.setPoison(false);
  CHECK(dealt > 0);
}

}  // namespace

int main() {
  if (!loadImage(gPath)) {
    gPath = "fixtures/sim-fixture.pack";
    if (!loadImage(gPath)) {
      std::printf("  no fixture pack\n");
      return 1;
    }
  }
  std::printf("  %s\n", gPath);
  CHECK(gMem.open(gImage.data(), static_cast<uint32_t>(gImage.size())) == pk::PackStatus::Ok);
  CHECK(gFile.open(gPath, 8));
  gSd.setArena(gArena, sizeof gArena);
  CHECK(gSd.open(gFile) == pk::PackStatus::Ok);
  if (tinta_test::failures() > 0) return tinta_test::result();

  testStrings();
  testRecords();
  testSearches();
  testArena();
  testWordOrderSurvivesPasses();
  std::printf("  %u block reads\n", gFile.rawReads());
  return tinta_test::result();
}

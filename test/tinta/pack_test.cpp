// modules: pack
//
// Checks src/core/pack against the Python reference implementation in
// tools/packc, on the pack compiled from test/fixtures/content-mini:
//  - every record of every section, through every accessor, against
//    tools/packc/reader.py (build/test/mini.expect.txt, written by
//    tools/packc/tests/pack_expect.py), at an aligned and at an odd base address;
//  - every key of LKEY, EKEY and FORM through the exact and prefix lookups;
//  - foldKey against `fold.py --vectors`, including every truncation length,
//    and against every lemma's LKEY key;
//  - stable facts of the fixture, written by hand;
//  - rejection of corrupt packs, the CRC, and a future-minor pack (wider
//    stride, unknown section).
// test/host/pack_test_prepare.sh builds the inputs.

#include "core/pack/Pack.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "core/pack/Fold.h"

using tinta::core::foldKey;
using tinta::core::ItemKind;
namespace tp = tinta::core::pack;
using tp::KeyRange;
using tp::kNone16;
using tp::kNone32;
using tp::Pack;
using tp::PackStatus;
using tp::Section;

namespace {

constexpr const char* kPackPath = "fixtures/mini.pack";
constexpr const char* kExpectPath = "fixtures/mini.expect.txt";
constexpr const char* kVectorsPath = "fixtures/fold_vectors.txt";

// ---- Input files ------------------------------------------------------------

std::vector<uint8_t> readFile(const char* path) {
  std::vector<uint8_t> data;
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return data;
  uint8_t chunk[4096];
  size_t n = 0;
  while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) data.insert(data.end(), chunk, chunk + n);
  std::fclose(f);
  return data;
}

std::vector<std::string> readLines(const char* path) {
  std::vector<std::string> lines;
  const std::vector<uint8_t> data = readFile(path);
  std::string line;
  for (uint8_t c : data) {
    if (c == '\n') {
      lines.push_back(line);
      line.clear();
    } else {
      line.push_back(static_cast<char>(c));
    }
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

std::string unhex(const std::string& hex) {
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<char>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
  }
  return out;
}

// One line of mini.expect.txt: TAG KEY name=value ... [text]
struct Expect {
  std::string tag;
  std::string key;
  std::vector<std::pair<std::string, long long>> fields;
  std::string text;

  uint32_t index() const { return static_cast<uint32_t>(std::strtoul(key.c_str(), nullptr, 10)); }
  long long get(const char* name) const {
    for (const auto& f : fields) {
      if (f.first == name) return f.second;
    }
    std::printf("  %s %s: no field %s\n", tag.c_str(), key.c_str(), name);
    ++tinta_test::failures();
    return -1;
  }
};

std::vector<Expect> parseExpect(const std::vector<std::string>& lines) {
  std::vector<Expect> out;
  for (const std::string& line : lines) {
    Expect e;
    size_t pos = 0;
    int word = 0;
    while (pos < line.size()) {
      size_t end = line.find(' ', pos);
      if (end == std::string::npos) end = line.size();
      const std::string token = line.substr(pos, end - pos);
      pos = end + 1;
      const size_t eq = token.find('=');
      if (word == 0) {
        e.tag = token;
      } else if (word == 1) {
        e.key = token;
      } else if (eq != std::string::npos) {
        e.fields.emplace_back(token.substr(0, eq), std::strtoll(token.c_str() + eq + 1, nullptr, 10));
      } else {
        e.text = token;
      }
      ++word;
    }
    out.push_back(e);
  }
  return out;
}

std::vector<Expect> gExpect;
std::vector<uint32_t> gLexs, gDist;  // LEXS and DIST as the Python reader sees them

// ---- Field-by-field comparison ----------------------------------------------

struct Field {
  const char* name;
  long long value;
};
#define FIELD(x) \
  Field { #x, static_cast<long long>(r.x) }

// Every name the Python reader printed must be a C++ field with the same
// value, and every C++ field must be named by the reader (reserved fields
// excepted: the reader drops some).
void checkFields(const Expect& e, const Field* fields, size_t n) {
  std::vector<bool> used(n, false);
  for (const auto& f : e.fields) {
    if (f.first.compare(0, 6, "forms[") == 0) continue;  // VERB grid, checked separately
    size_t k = 0;
    while (k < n && f.first != fields[k].name) ++k;
    if (k == n) {
      std::printf("  %s %s: no C++ field named %s\n", e.tag.c_str(), e.key.c_str(), f.first.c_str());
      ++tinta_test::failures();
      continue;
    }
    used[k] = true;
    if (fields[k].value != f.second) {
      std::printf("  %s %s: %s == %lld, reader.py says %lld\n", e.tag.c_str(), e.key.c_str(), fields[k].name,
                  fields[k].value, f.second);
      ++tinta_test::failures();
    }
  }
  for (size_t k = 0; k < n; ++k) {
    if (!used[k] && std::strcmp(fields[k].name, "reserved") != 0) {
      std::printf("  %s %s: C++ field %s not in reader.py output\n", e.tag.c_str(), e.key.c_str(), fields[k].name);
      ++tinta_test::failures();
    }
  }
}

template <size_t N>
void checkFields(const Expect& e, const Field (&fields)[N]) {
  checkFields(e, fields, N);
}

void check(const Expect& e, const tp::Lemma& r) {
  const Field f[] = {FIELD(es),       FIELD(en),        FIELD(pron),         FIELD(note),         FIELD(alt),
                     FIELD(feminine), FIELD(plural),    FIELD(firstExample), FIELD(exampleCount), FIELD(freqRank),
                     FIELD(lesson),   FIELD(verbTable), FIELD(pos),          FIELD(gender),       FIELD(level),
                     FIELD(reg),      FIELD(flags),     FIELD(reserved)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::LemmaKey& r) {
  const Field f[] = {FIELD(key), FIELD(lemma), FIELD(reserved)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::EnglishKey& r) {
  const Field f[] = {FIELD(key), FIELD(lemma), FIELD(rank), FIELD(reserved)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::FormKey& r) {
  const Field f[] = {FIELD(key), FIELD(form), FIELD(lemma), FIELD(tag)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::VerbInfo& r) {
  const Field f[] = {FIELD(infinitive), FIELD(gerund), FIELD(participle), FIELD(lemma), FIELD(flags)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::Sentence& r) {
  const Field f[] = {FIELD(es),    FIELD(en),     FIELD(note),  FIELD(firstToken), FIELD(lesson),  FIELD(tokenCount),
                     FIELD(level), FIELD(source), FIELD(flags), FIELD(reg),        FIELD(reserved)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::Token& r) {
  const Field f[] = {FIELD(start), FIELD(length), FIELD(flags), FIELD(lemma), FIELD(tag)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::Item& r) {
  const Field f[] = {FIELD(uid), FIELD(kind),   FIELD(candidateCount), FIELD(lesson),   FIELD(a),
                     FIELD(b),   FIELD(prereq), FIELD(flags),          FIELD(reserved), FIELD(firstCandidate)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::Unit& r) {
  const Field f[] = {FIELD(title),       FIELD(titleEn),     FIELD(goals),   FIELD(number),
                     FIELD(firstLesson), FIELD(lessonCount), FIELD(reserved)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::Lesson& r) {
  const Field f[] = {FIELD(title),         FIELD(titleEn),   FIELD(firstItem), FIELD(itemCount), FIELD(unit),
                     FIELD(number),        FIELD(firstNote), FIELD(noteCount), FIELD(dialogue),  FIELD(firstSentence),
                     FIELD(sentenceCount), FIELD(newCount),  FIELD(level),     FIELD(flags)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::Note& r) {
  const Field f[] = {FIELD(title), FIELD(firstSpan), FIELD(spanCount), FIELD(lesson),
                     FIELD(kind),  FIELD(flags),     FIELD(reserved)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::NoteSpan& r) {
  const Field f[] = {FIELD(text), FIELD(style), FIELD(flags), FIELD(reserved)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::Story& r) {
  const Field f[] = {FIELD(title),  FIELD(titleEn),       FIELD(firstLine),     FIELD(lineCount),
                     FIELD(lesson), FIELD(firstQuestion), FIELD(questionCount), FIELD(level),
                     FIELD(kind),   FIELD(flags),         FIELD(reserved)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::StoryLine& r) {
  const Field f[] = {FIELD(speaker), FIELD(sentence), FIELD(flags)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::Question& r) {
  const Field f[] = {FIELD(text),   FIELD(options[0]),  FIELD(options[1]), FIELD(options[2]), FIELD(options[3]),
                     FIELD(answer), FIELD(optionCount), FIELD(flags),      FIELD(reserved)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::PhraseCategory& r) {
  const Field f[] = {FIELD(title), FIELD(titleEn), FIELD(firstEntry), FIELD(entryCount)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::PhraseEntry& r) {
  const Field f[] = {FIELD(pron), FIELD(sentence), FIELD(category)};
  checkFields(e, f);
}
void check(const Expect& e, const tp::ConfusableSet& r) {
  const Field f[] = {FIELD(name),       FIELD(note),       FIELD(members[0]), FIELD(members[1]),  FIELD(members[2]),
                     FIELD(members[3]), FIELD(members[4]), FIELD(members[5]), FIELD(memberCount), FIELD(reserved)};
  checkFields(e, f);
}

template <class T>
bool sameBytes(const T& a, const T& b) {
  return std::memcmp(&a, &b, sizeof(T)) == 0;
}

bool contains(const KeyRange& r, uint32_t i) { return i >= r.first && i - r.first < r.count; }

// ---- Lookups ----------------------------------------------------------------

template <class Rec>
using AtFn = bool (Pack::*)(uint32_t, Rec&) const;
using FindFn = KeyRange (Pack::*)(const char*) const;

template <class Rec>
const char* keyOf(const Pack& p, AtFn<Rec> at, uint32_t j) {
  Rec rec{};
  return (p.*at)(j, rec) ? p.str(rec.key) : nullptr;
}

// Key i of a sorted table must be found by its own text through the exact
// lookup and through every prefix of it, and each range must be exactly the
// keys that match.
template <class Rec>
void checkLookups(const Pack& p, Section s, uint32_t i, FindFn exact, FindFn prefix, AtFn<Rec> at) {
  const uint32_t total = p.count(s);
  const char* own = keyOf(p, at, i);
  CHECK(own != nullptr);
  if (own == nullptr) return;
  const std::string key = own;
  // A cut key can end in a space, which folding drops ("be " -> "be"), so
  // the expected matches are those of the folded query.
  const auto checkRange = [&](const KeyRange& r, const std::string& query, bool isPrefix) {
    char folded[Pack::kMaxQuery];
    foldKey(query.c_str(), folded, sizeof folded);
    const size_t len = std::strlen(folded);
    const auto matches = [&](uint32_t j) {
      const char* k = keyOf(p, at, j);
      if (k == nullptr) return false;
      return isPrefix ? std::strncmp(k, folded, len) == 0 : std::strcmp(k, folded) == 0;
    };
    if (!contains(r, i)) {
      std::printf("  key %u \"%s\": %s \"%s\" gives range %u+%u\n", i, key.c_str(), isPrefix ? "prefix" : "exact",
                  query.c_str(), r.first, r.count);
      ++tinta_test::failures();
      return;
    }
    for (uint32_t j = r.first; j < r.first + r.count; ++j) CHECK(matches(j));
    if (r.first > 0) CHECK(!matches(r.first - 1));
    if (r.first + r.count < total) CHECK(!matches(r.first + r.count));
  };
  checkRange((p.*exact)(key.c_str()), key, false);
  if (prefix != nullptr) {
    for (size_t len = 0; len <= key.size(); ++len) {
      const std::string query = key.substr(0, len);
      checkRange((p.*prefix)(query.c_str()), query, true);
    }
  }
}

// ---- Cross-check against reader.py ------------------------------------------

Section sectionOf(const std::string& tag) {
  for (size_t s = 0; s < tp::kSectionCount; ++s) {
    if (tp::kSections[s].tag == tp::makeTag({tag[0], tag[1], tag[2], tag[3], 0})) {
      return static_cast<Section>(s);
    }
  }
  std::printf("  unknown section %s\n", tag.c_str());
  ++tinta_test::failures();
  return Section::Strs;
}

void crossCheck(const Pack& p) {
  CHECK(p.isOpen());
  uint32_t strings = 0;
  for (const Expect& e : gExpect) {
    const uint32_t i = e.index();
    const std::string& t = e.tag;
    if (t == "HEAD") {
      CHECK_EQ(p.formatMajor(), e.get("formatMajor"));
      CHECK_EQ(p.formatMinor(), e.get("formatMinor"));
      CHECK_EQ(p.contentVersion(), e.get("contentVersion"));
      CHECK_EQ(p.buildTime(), e.get("buildTime"));
      CHECK_EQ(p.isRelease(), (e.get("flags") & tp::kHeaderRelease) != 0);
    } else if (t == "LOCL") {
      CHECK_STR_EQ(p.locale(), e.text.c_str());
    } else if (t == "SECT") {
      CHECK_EQ(p.count(sectionOf(e.key)), e.get("count"));
    } else if (t == "STRS") {
      ++strings;
      const std::string want = unhex(e.text);
      CHECK_STR_EQ(p.str(i), want.c_str());
    } else if (t == "LEXS" || t == "DIST") {
      // Checked through their parents below.
    } else if (t == "LEMM") {
      tp::Lemma r;
      CHECK(p.lemma(static_cast<uint16_t>(i), r));
      check(e, r);
      for (uint16_t n = 0; n < r.exampleCount; ++n) {
        CHECK_EQ(p.lemmaExample(r, n), gLexs.at(r.firstExample + n));
      }
      CHECK_EQ(p.lemmaExample(r, r.exampleCount), kNone16);
    } else if (t == "LKEY") {
      tp::LemmaKey r;
      CHECK(p.lemmaKeyAt(i, r));
      check(e, r);
      checkLookups<tp::LemmaKey>(p, Section::Lkey, i, &Pack::findLemma, &Pack::findLemmaPrefix, &Pack::lemmaKeyAt);
      // The device's fold of the headword gives the key the compiler stored.
      tp::Lemma lem;
      CHECK(p.lemma(r.lemma, lem));
      char folded[Pack::kMaxQuery];
      foldKey(p.str(lem.es), folded, sizeof folded);
      CHECK_STR_EQ(folded, p.str(r.key));
    } else if (t == "EKEY") {
      tp::EnglishKey r;
      CHECK(p.englishAt(i, r));
      check(e, r);
      checkLookups<tp::EnglishKey>(p, Section::Ekey, i, &Pack::findEnglish, &Pack::findEnglishPrefix, &Pack::englishAt);
    } else if (t == "FORM") {
      tp::FormKey r;
      CHECK(p.formAt(i, r));
      check(e, r);
      checkLookups<tp::FormKey>(p, Section::Form, i, &Pack::findForm, nullptr, &Pack::formAt);
    } else if (t == "VERB") {
      const auto table = static_cast<uint16_t>(i);
      tp::VerbInfo r;
      CHECK(p.verbInfo(table, r));
      check(e, r);
      for (uint8_t tense = 0; tense < tp::kTenseCount; ++tense) {
        for (uint8_t person = 0; person < tp::kPersonCount; ++person) {
          char name[16];
          std::snprintf(name, sizeof name, "forms[%u][%u]", static_cast<unsigned>(tense),
                        static_cast<unsigned>(person));
          const auto offset = static_cast<uint32_t>(e.get(name));
          const auto te = static_cast<tp::Tense>(tense);
          const auto pe = static_cast<tp::Person>(person);
          const char* got = p.verbForm(table, te, pe);
          CHECK_STR_EQ(got, p.str(offset));
          if (offset != 0) CHECK(got == p.str(offset));
          const uint16_t tag = tp::makeVerbTag(te, pe);
          CHECK_STR_EQ(p.verbForm(table, tag), got);
          CHECK_STR_EQ(p.verbForm(table, static_cast<uint16_t>(tag | tp::kTagEnclitic)), got);
        }
      }
      CHECK_STR_EQ(p.verbForm(table, tp::kTagInfinitive), p.str(r.infinitive));
      CHECK_STR_EQ(p.verbForm(table, tp::kTagGerund), p.str(r.gerund));
      CHECK_STR_EQ(p.verbForm(table, tp::kTagParticiple), p.str(r.participle));
      CHECK_STR_EQ(p.verbForm(table, tp::kTagParticipleFeminine), "");
      CHECK_STR_EQ(p.verbForm(table, tp::kTagPlural), "");
      CHECK_STR_EQ(p.verbForm(table, tp::kTagBase), "");
      CHECK_STR_EQ(p.verbForm(table, static_cast<uint16_t>(0x1085)), "");  // person 5
      CHECK_STR_EQ(p.verbForm(table, static_cast<uint16_t>(0x1080)), "");  // tense 8
      CHECK_STR_EQ(p.verbForm(table, static_cast<tp::Tense>(8), tp::Person::Yo), "");
      CHECK_STR_EQ(p.verbForm(table, tp::Tense::Present, static_cast<tp::Person>(5)), "");
    } else if (t == "SENT") {
      tp::Sentence r;
      CHECK(p.sentence(static_cast<uint16_t>(i), r));
      check(e, r);
      const char* es = p.str(r.es);
      for (uint8_t k = 0; k < r.tokenCount; ++k) {
        tp::Token viaParent, global;
        CHECK(p.token(r, k, viaParent));
        CHECK(p.token(r.firstToken + k, global));
        CHECK(sameBytes(viaParent, global));
        const tp::TextSpan span = p.tokenText(r, viaParent);
        CHECK(span.text == es + viaParent.start);
        CHECK_EQ(span.length, viaParent.length);
      }
      tp::Token past;
      CHECK(!p.token(r, r.tokenCount, past));
    } else if (t == "TOKS") {
      tp::Token r;
      CHECK(p.token(i, r));
      check(e, r);
    } else if (t == "ITEM") {
      tp::Item r;
      CHECK(p.item(i, r));
      check(e, r);
      CHECK_EQ(p.uidAt(i), r.uid);
      CHECK_EQ(p.kindAt(i), r.kind);
      CHECK_EQ(p.lessonAt(i), r.lesson);
      CHECK_EQ(p.prerequisiteOf(i), r.prereq == kNone16 ? -1 : r.prereq);
      for (uint8_t n = 0; n < r.candidateCount; ++n) {
        CHECK_EQ(p.itemCandidate(r, n), gDist.at(r.firstCandidate + n));
      }
      CHECK_EQ(p.itemCandidate(r, r.candidateCount), kNone32);
    } else if (t == "IUID") {
      CHECK_EQ(p.indexOfUid(static_cast<uint32_t>(e.get("uid"))), e.get("index"));
    } else if (t == "UNIT") {
      tp::Unit r;
      CHECK(p.unit(static_cast<uint16_t>(i), r));
      check(e, r);
    } else if (t == "LESS") {
      tp::Lesson r;
      CHECK(p.lesson(static_cast<uint16_t>(i), r));
      check(e, r);
    } else if (t == "NOTE") {
      tp::Note r;
      CHECK(p.note(static_cast<uint16_t>(i), r));
      check(e, r);
      for (uint16_t k = 0; k < r.spanCount; ++k) {
        tp::NoteSpan viaParent, global;
        CHECK(p.noteSpan(r, k, viaParent));
        CHECK(p.noteSpan(r.firstSpan + k, global));
        CHECK(sameBytes(viaParent, global));
      }
      tp::NoteSpan past;
      CHECK(!p.noteSpan(r, r.spanCount, past));
    } else if (t == "NSPN") {
      tp::NoteSpan r;
      CHECK(p.noteSpan(i, r));
      check(e, r);
    } else if (t == "STOR") {
      tp::Story r;
      CHECK(p.story(static_cast<uint16_t>(i), r));
      check(e, r);
      for (uint16_t k = 0; k < r.lineCount; ++k) {
        tp::StoryLine viaParent, global;
        CHECK(p.storyLine(r, k, viaParent));
        CHECK(p.storyLine(r.firstLine + k, global));
        CHECK(sameBytes(viaParent, global));
      }
      for (uint8_t k = 0; k < r.questionCount; ++k) {
        tp::Question viaParent, global;
        CHECK(p.question(r, k, viaParent));
        CHECK(p.question(r.firstQuestion + k, global));
        CHECK(sameBytes(viaParent, global));
      }
      tp::StoryLine pastLine;
      tp::Question pastQuestion;
      CHECK(!p.storyLine(r, r.lineCount, pastLine));
      CHECK(!p.question(r, r.questionCount, pastQuestion));
    } else if (t == "SLIN") {
      tp::StoryLine r;
      CHECK(p.storyLine(i, r));
      check(e, r);
    } else if (t == "SQST") {
      tp::Question r;
      CHECK(p.question(i, r));
      check(e, r);
    } else if (t == "PHRS") {
      tp::PhraseCategory r;
      CHECK(p.phraseCategory(static_cast<uint16_t>(i), r));
      check(e, r);
      for (uint16_t k = 0; k < r.entryCount; ++k) {
        tp::PhraseEntry viaParent, global;
        CHECK(p.phraseEntry(r, k, viaParent));
        CHECK(p.phraseEntry(r.firstEntry + k, global));
        CHECK(sameBytes(viaParent, global));
      }
      tp::PhraseEntry past;
      CHECK(!p.phraseEntry(r, r.entryCount, past));
    } else if (t == "PENT") {
      tp::PhraseEntry r;
      CHECK(p.phraseEntry(i, r));
      check(e, r);
    } else if (t == "CONF") {
      tp::ConfusableSet r;
      CHECK(p.confusable(static_cast<uint16_t>(i), r));
      check(e, r);
    } else {
      std::printf("  unexpected expectation line %s %s\n", t.c_str(), e.key.c_str());
      ++tinta_test::failures();
    }
  }
  CHECK_EQ(strings + 1, p.count(Section::Strs));  // + the empty string at offset 0
  CHECK_EQ(gLexs.size(), p.count(Section::Lexs));
  CHECK_EQ(gDist.size(), p.count(Section::Dist));
  CHECK_EQ(p.count(Section::Iuid), p.itemCount());

  // Every getter refuses the first id past its section and zeroes the output.
  tp::Lemma lem;
  std::memset(&lem, 0xAB, sizeof lem);
  CHECK(!p.lemma(static_cast<uint16_t>(p.count(Section::Lemm)), lem));
  CHECK_EQ(lem.es, 0u);
  CHECK_EQ(lem.flags, 0);
  CHECK(!p.lemma(kNone16, lem));
  tp::LemmaKey lk;
  tp::EnglishKey ek;
  tp::FormKey fk;
  tp::VerbInfo vi;
  tp::Sentence se;
  tp::Token tok;
  tp::Item it;
  tp::Unit un;
  tp::Lesson le;
  tp::Note no;
  tp::NoteSpan ns;
  tp::Story st;
  tp::StoryLine sl;
  tp::Question qu;
  tp::PhraseCategory pc;
  tp::PhraseEntry pe;
  tp::ConfusableSet cs;
  CHECK(!p.lemmaKeyAt(p.count(Section::Lkey), lk));
  CHECK(!p.englishAt(p.count(Section::Ekey), ek));
  CHECK(!p.formAt(p.count(Section::Form), fk));
  CHECK(!p.verbInfo(static_cast<uint16_t>(p.count(Section::Verb)), vi));
  CHECK_STR_EQ(p.verbForm(static_cast<uint16_t>(p.count(Section::Verb)), tp::Tense::Present, tp::Person::Yo), "");
  CHECK(!p.sentence(static_cast<uint16_t>(p.count(Section::Sent)), se));
  CHECK(!p.token(p.count(Section::Toks), tok));
  CHECK(!p.item(p.itemCount(), it));
  CHECK(!p.unit(static_cast<uint16_t>(p.count(Section::Unit)), un));
  CHECK(!p.lesson(static_cast<uint16_t>(p.count(Section::Less)), le));
  CHECK(!p.note(static_cast<uint16_t>(p.count(Section::Note)), no));
  CHECK(!p.noteSpan(p.count(Section::Nspn), ns));
  CHECK(!p.story(static_cast<uint16_t>(p.count(Section::Stor)), st));
  CHECK(!p.storyLine(p.count(Section::Slin), sl));
  CHECK(!p.question(p.count(Section::Sqst), qu));
  CHECK(!p.phraseCategory(static_cast<uint16_t>(p.count(Section::Phrs)), pc));
  CHECK(!p.phraseEntry(p.count(Section::Pent), pe));
  CHECK(!p.confusable(static_cast<uint16_t>(p.count(Section::Conf)), cs));
  CHECK_EQ(p.uidAt(p.itemCount()), 0u);
  CHECK_EQ(p.lessonAt(p.itemCount()), kNone16);
  CHECK_EQ(p.prerequisiteOf(p.itemCount()), -1);
  CHECK_STR_EQ(p.str(0), "");
  CHECK_STR_EQ(p.str(0xFFFFFFFFu), "");

  // A parent whose run points past its section reads nothing, and a run
  // whose start + index would wrap 32 bits is refused rather than wrapped.
  tp::Sentence bogus{};
  bogus.firstToken = p.count(Section::Toks);
  bogus.tokenCount = 3;
  CHECK(!p.token(bogus, 0, tok));
  bogus.firstToken = 0xFFFFFFFFu;
  CHECK(!p.token(bogus, 2, tok));
  tp::Lemma bogusLemma{};
  bogusLemma.firstExample = p.count(Section::Lexs) - 1;
  bogusLemma.exampleCount = 5;
  CHECK(bogusLemma.firstExample < p.count(Section::Lexs));
  CHECK_EQ(p.lemmaExample(bogusLemma, 1), kNone16);
  tp::Item bogusItem{};
  bogusItem.firstCandidate = 0xFFFFFFF0u;
  bogusItem.candidateCount = 200;
  CHECK_EQ(p.itemCandidate(bogusItem, 100), kNone32);
  // A token span past the end of its sentence is clamped to the string.
  CHECK(p.sentence(0, se));
  tp::Token wild{};
  wild.start = 2;
  wild.length = 255;
  const tp::TextSpan clamped = p.tokenText(se, wild);
  CHECK_EQ(clamped.length, std::strlen(p.str(se.es)) - 2);
  wild.start = 60000;
  CHECK_EQ(p.tokenText(se, wild).length, 0u);
}

// ---- Fold ---------------------------------------------------------------------

void checkFold() {
  const std::vector<std::string> lines = readLines(kVectorsPath);
  CHECK(lines.size() >= 200);
  for (const std::string& line : lines) {
    const size_t tab = line.find('\t');
    CHECK(tab != std::string::npos);
    if (tab == std::string::npos) continue;
    const std::string in = unhex(line.substr(0, tab));
    const std::string want = unhex(line.substr(tab + 1));
    CHECK(in.find('\0') == std::string::npos);
    char out[256];
    const size_t n = foldKey(in.c_str(), out, sizeof out);
    if (n != want.size() || want != out) {
      std::printf("  fold(%s) == \"%s\" (%zu), fold.py says \"%s\"\n", line.substr(0, tab).c_str(), out, n,
                  want.c_str());
      ++tinta_test::failures();
    }
    // Every cut is the same-length prefix of Python's key, and nothing is
    // written past outSize.
    for (size_t size = 0; size <= want.size() + 1; ++size) {
      char small[260];
      std::memset(small, '#', sizeof small);
      CHECK_EQ(foldKey(in.c_str(), small, size), want.size());
      const std::string cutWant = size > 0 ? want.substr(0, size - 1) : std::string();
      if (size > 0) CHECK_STR_EQ(small, cutWant.c_str());
      CHECK(small[size] == '#');
    }
  }

  char out[32];
  const struct {
    const char* in;
    const char* want;
  } cases[] = {
      {"a\xFF"
       "b",
       "ab"},            // invalid byte
      {"ni\xC3", "ni"},  // truncated sequence at the end
      {"\xC3"
       "a",
       "a"},  // lead byte without continuation
      {"\xC0\xAF"
       "x",
       "x"},                 // overlong '/'
      {"\xE0\x80\xAF", ""},  // overlong, 3 bytes
      {"\xED\xA0\x80"
       "z",
       "z"},                         // surrogate
      {"\xF4\x90\x80\x80", ""},      // above U+10FFFF
      {"x\xF0\x9F\x99\x82y", "xy"},  // 4-byte sequence, dropped
      {"e\xCC\x81", "e"},            // decomposed é: the mark is dropped
      {"Ni\xC3\xB1o", "nino"},
      {"\xC2\xBF"
       "A\xC2\xA0"
       "b?",
       "ab"},  // NBSP is not a separator
      {" \t\n", ""},
      {"\xC2\xBF a", "a"},  // dropped char before a separator
      {"a \xC2\xBF"
       "b",
       "a b"},                   // dropped char after a separator
      {"\xC3\x97\xC3\xB7", ""},  // × ÷
      {"\xC2\x97"
       "De",
       "de"},  // C1 slot (em dash) dropped
  };
  for (const auto& c : cases) {
    const size_t n = foldKey(c.in, out, sizeof out);
    CHECK_STR_EQ(out, c.want);
    CHECK_EQ(n, std::strlen(c.want));
  }
  CHECK_EQ(foldKey(nullptr, out, sizeof out), 0u);
  CHECK_STR_EQ(out, "");
  CHECK_EQ(foldKey("Stra\xC3\x9F"
                   "e",
                   nullptr, 0),
           7u);
  CHECK_EQ(foldKey("\xC3\x9F", out, 2), 2u);  // ß cut after its first letter, as Python's [:1]
  CHECK_STR_EQ(out, "s");
  CHECK_EQ(foldKey("a  b", out, 3), 3u);  // cut on the separator
  CHECK_STR_EQ(out, "a ");
}

// ---- Fixture facts ------------------------------------------------------------

// The lemma whose headword is exactly `headword`, found through LKEY.
uint16_t lemmaId(const Pack& p, const char* headword) {
  const KeyRange r = p.findLemma(headword);
  for (uint32_t j = r.first; j < r.first + r.count; ++j) {
    tp::LemmaKey k;
    tp::Lemma lem;
    if (p.lemmaKeyAt(j, k) && p.lemma(k.lemma, lem) && std::strcmp(p.str(lem.es), headword) == 0) {
      return k.lemma;
    }
  }
  std::printf("  no lemma \"%s\"\n", headword);
  ++tinta_test::failures();
  return kNone16;
}

std::vector<uint16_t> lemmasIn(const Pack& p, const KeyRange& r) {
  std::vector<uint16_t> ids;
  for (uint32_t j = r.first; j < r.first + r.count; ++j) {
    tp::LemmaKey k;
    CHECK(p.lemmaKeyAt(j, k));
    ids.push_back(k.lemma);
  }
  return ids;
}

bool has(const std::vector<uint16_t>& ids, uint16_t id) {
  for (uint16_t x : ids) {
    if (x == id) return true;
  }
  return false;
}

int32_t findItem(const Pack& p, ItemKind kind, uint16_t a, uint16_t b) {
  for (uint32_t i = 0; i < p.itemCount(); ++i) {
    tp::Item it;
    if (p.item(i, it) && it.kind == kind && it.a == a && it.b == b) return static_cast<int32_t>(i);
  }
  return -1;
}

uint16_t sentenceId(const Pack& p, const char* es) {
  for (uint32_t i = 0; i < p.count(Section::Sent); ++i) {
    tp::Sentence s;
    if (p.sentence(static_cast<uint16_t>(i), s) && std::strcmp(p.str(s.es), es) == 0) {
      return static_cast<uint16_t>(i);
    }
  }
  std::printf("  no sentence \"%s\"\n", es);
  ++tinta_test::failures();
  return kNone16;
}

std::string tokenString(const Pack& p, const tp::Sentence& s, uint8_t k) {
  tp::Token t;
  if (!p.token(s, k, t)) return "<none>";
  const tp::TextSpan span = p.tokenText(s, t);
  return std::string(span.text, span.length);
}

#define CHECK_TOKEN(p, sentence, k, want)                        \
  do {                                                           \
    const std::string tinta_token = tokenString(p, sentence, k); \
    CHECK_STR_EQ(tinta_token.c_str(), want);                     \
  } while (0)

void checkFixtureFacts(const Pack& p) {
  // Header.
  CHECK_EQ(p.formatMajor(), 1);
  CHECK_EQ(p.formatMinor(), tp::kFormatMinor);
  CHECK_EQ(p.contentVersion(), 7u);
  CHECK_STR_EQ(p.locale(), "es-MX");
  CHECK(!p.isRelease());
  CHECK(p.verifyCrc());

  // agua: feminine, takes el, plural aguas; its first example is the sentence
  // where it is the marked target.
  tp::Lemma lem;
  const uint16_t agua = lemmaId(p, "agua");
  CHECK(p.lemma(agua, lem));
  CHECK_EQ(lem.pos, tp::PartOfSpeech::Noun);
  CHECK_EQ(lem.gender, tp::Gender::Feminine);
  CHECK((lem.flags & tp::kLemmaElFeminine) != 0);
  CHECK((lem.flags & tp::kLemmaHasItems) != 0);
  CHECK_STR_EQ(p.str(lem.plural), "aguas");
  CHECK_EQ(lem.verbTable, kNone16);
  CHECK_EQ(lem.lesson, 1);
  tp::Sentence sent;
  CHECK(lem.exampleCount >= 2);
  CHECK(p.sentence(p.lemmaExample(lem, 0), sent));
  CHECK_STR_EQ(p.str(sent.es),
               "El agua est\xC3\xA1 muy fr\xC3\xAD"
               "a.");
  tp::Lemma other;
  CHECK(p.lemma(lemmaId(p, "casa"), other));
  CHECK((other.flags & tp::kLemmaElFeminine) == 0);

  // Lexicon columns.
  CHECK(p.lemma(lemmaId(p, "cami\xC3\xB3n"), lem));
  CHECK((lem.flags & tp::kLemmaMexico) != 0);
  CHECK_EQ(lem.gender, tp::Gender::Masculine);
  CHECK_STR_EQ(p.str(lem.alt), "autob\xC3\xBAs");
  CHECK_STR_EQ(p.str(lem.note), "In Mexico City also a truck.");
  CHECK(p.lemma(lemmaId(p, "lentes"), lem));
  CHECK((lem.flags & tp::kLemmaPluralOnly) != 0);
  CHECK(p.lemma(lemmaId(p, "amigo"), lem));
  CHECK_EQ(lem.gender, tp::Gender::Both);
  CHECK_STR_EQ(p.str(lem.feminine), "amiga");
  CHECK(p.lemma(lemmaId(p, "chamba"), lem));
  CHECK_EQ(lem.reg, tp::Register::Informal);
  CHECK_EQ(lem.level, tp::Level::A2);
  CHECK(p.lemma(lemmaId(p,
                        "d\xC3\xAD"
                        "a"),
                lem));
  CHECK((lem.flags & tp::kLemmaFrequencyDeck) != 0);
  CHECK_EQ(lem.freqRank, 40);
  CHECK_EQ(lem.lesson, kNone16);
  CHECK(p.lemma(lemmaId(p, "\xC2\xBFmande?"), lem));
  CHECK_EQ(lem.pos, tp::PartOfSpeech::Expression);

  // México: found with or without accents and case; the fixture overrides
  // its respelling.
  const uint16_t mexico = lemmaId(p, "M\xC3\xA9xico");
  for (const char* q : {"mexico", "M\xC3\xA9xico", "M\xC3\x89XICO", "  m\xC3\xA9xico! "}) {
    const KeyRange r = p.findLemma(q);
    CHECK_EQ(r.count, 1u);
    CHECK(has(lemmasIn(p, r), mexico));
  }
  CHECK(p.lemma(mexico, lem));
  CHECK_STR_EQ(p.str(lem.pron), "MEH-hee-koh");
  CHECK_EQ(lem.pos, tp::PartOfSpeech::ProperNoun);
  CHECK_EQ(p.findLemma("mexic").count, 0u);
  CHECK_EQ(p.findLemma("").count, 0u);

  // Two lemmas share the key "papa" (potato, pope).
  CHECK_EQ(p.findLemma("papa").count, 2u);

  // Search-as-you-type.
  const KeyRange co = p.findLemmaPrefix("co");
  const std::vector<uint16_t> coIds = lemmasIn(p, co);
  CHECK(has(coIds, lemmaId(p, "comer")));
  CHECK(has(coIds, lemmaId(p, "computadora")));
  CHECK(has(coIds, lemmaId(p, "con permiso")));
  CHECK(has(coIds, lemmaId(p, "c\xC3\xB3mo")));
  CHECK(has(coIds, lemmaId(p, "con")));
  CHECK(!has(coIds, lemmaId(p, "casa")));
  CHECK_EQ(p.findLemmaPrefix("C\xC3\x93").count, co.count);  // "CÓ" folds to "co"
  CHECK_EQ(p.findLemmaPrefix("con p").count, 1u);            // con permiso, not con
  // Folding drops a trailing space, so "con " still lists con itself.
  CHECK_EQ(p.findLemmaPrefix("con ").count, p.findLemmaPrefix("con").count);
  CHECK_EQ(p.findLemmaPrefix("").count, p.count(Section::Lemm));
  CHECK_EQ(p.findLemmaPrefix("\xC2\xBF").count, p.count(Section::Lemm));  // "¿" folds to ""
  CHECK_EQ(p.findLemmaPrefix("zz").count, 0u);
  CHECK_EQ(p.findLemmaPrefix("zz").first, p.count(Section::Lkey));
  // A query whose key does not fit kMaxQuery matches nothing.
  const std::string longQuery(Pack::kMaxQuery, 'a');
  CHECK_EQ(p.findLemmaPrefix(longQuery.c_str()).count, 0u);
  CHECK_EQ(p.findLemmaPrefix(longQuery.substr(0, Pack::kMaxQuery - 2).c_str()).count, 0u);

  // Alphabetical browse: LKEY is sorted and, as the format promises, lemma
  // ids are in LKEY order.
  for (uint32_t j = 0; j < p.count(Section::Lkey); ++j) {
    tp::LemmaKey k;
    CHECK(p.lemmaKeyAt(j, k));
    CHECK_EQ(k.lemma, j);
    if (j > 0) {
      tp::LemmaKey prev;
      CHECK(p.lemmaKeyAt(j - 1, prev));
      CHECK(std::strcmp(p.str(prev.key), p.str(k.key)) <= 0);
    }
  }
  // Letter jump: the first lemma at or after "m".
  const KeyRange m = p.findLemmaPrefix("m");
  CHECK(m.count > 0);
  tp::LemmaKey firstM;
  CHECK(p.lemmaKeyAt(m.first, firstM));
  CHECK(p.str(firstM.key)[0] == 'm');

  // Inflected forms.
  const uint16_t ser = lemmaId(p, "ser");
  const uint16_t estar = lemmaId(p, "estar");
  const KeyRange soy = p.findForm("soy");
  bool foundSoy = false;
  for (uint32_t j = soy.first; j < soy.first + soy.count; ++j) {
    tp::FormKey f;
    CHECK(p.formAt(j, f));
    if (f.lemma == ser && f.tag == tp::makeVerbTag(tp::Tense::Present, tp::Person::Yo)) {
      foundSoy = true;
      CHECK_STR_EQ(p.str(f.form), "soy");
    }
  }
  CHECK(foundSoy);
  const KeyRange esta = p.findForm("ESTA");  // está (pres.3s) and está (imp.2s)
  bool foundEsta = false;
  for (uint32_t j = esta.first; j < esta.first + esta.count; ++j) {
    tp::FormKey f;
    CHECK(p.formAt(j, f));
    CHECK_EQ(f.lemma, estar);
    CHECK_STR_EQ(p.str(f.form), "est\xC3\xA1");
    CHECK(tp::isVerbTag(f.tag));
    if (f.tag == tp::makeVerbTag(tp::Tense::Present, tp::Person::Usted)) foundEsta = true;
  }
  CHECK(foundEsta);
  const KeyRange aguas = p.findForm("aguas");
  CHECK_EQ(aguas.count, 1u);
  tp::FormKey fk;
  CHECK(p.formAt(aguas.first, fk));
  CHECK_EQ(fk.lemma, agua);
  CHECK_EQ(fk.tag, tp::kTagPlural);
  CHECK(tp::isNominalTag(fk.tag));
  const KeyRange buen = p.findForm("buen");
  CHECK_EQ(buen.count, 1u);
  CHECK(p.formAt(buen.first, fk));
  CHECK_EQ(fk.lemma, lemmaId(p, "bueno"));
  CHECK_EQ(fk.tag, tp::kTagApocope);
  CHECK_EQ(p.findForm("agua").count, 0u);  // headwords live in LKEY only

  // English.
  const KeyRange bus = p.findEnglish("Bus");
  CHECK_EQ(bus.count, 1u);
  tp::EnglishKey ek;
  CHECK(p.englishAt(bus.first, ek));
  CHECK_EQ(ek.lemma, lemmaId(p, "cami\xC3\xB3n"));
  CHECK_EQ(ek.rank, 0);
  const KeyRange be = p.findEnglish("be");  // "to be (...)": "to" and the parenthesis go
  CHECK_EQ(be.count, 2u);
  std::vector<uint16_t> beIds;
  for (uint32_t j = be.first; j < be.first + be.count; ++j) {
    CHECK(p.englishAt(j, ek));
    beIds.push_back(ek.lemma);
  }
  CHECK(has(beIds, ser) && has(beIds, estar));
  const KeyRange tha = p.findEnglishPrefix("tha");
  const uint16_t gracias = lemmaId(p, "gracias");
  CHECK(tha.count >= 2);
  for (uint32_t j = tha.first; j < tha.first + tha.count; ++j) {
    CHECK(p.englishAt(j, ek));
    CHECK_EQ(ek.lemma, gracias);
  }

  // Verb tables: ser and estar are written out in the fixture.
  CHECK(p.lemma(ser, lem));
  CHECK_EQ(lem.pos, tp::PartOfSpeech::Verb);
  tp::VerbInfo vi;
  CHECK(p.verbInfo(lem.verbTable, vi));
  CHECK_EQ(vi.lemma, ser);
  CHECK_STR_EQ(p.str(vi.infinitive), "ser");
  CHECK((vi.flags & tp::kVerbIrregular) != 0);
  CHECK_EQ(tp::verbStemChange(vi.flags), tp::StemChange::None);
  const char* serPresent[] = {"soy", "eres", "es", "somos", "son"};
  const char* serPreterite[] = {"fui", "fuiste", "fue", "fuimos", "fueron"};
  for (uint8_t person = 0; person < tp::kPersonCount; ++person) {
    const auto pe = static_cast<tp::Person>(person);
    CHECK_STR_EQ(p.verbForm(lem.verbTable, tp::Tense::Present, pe), serPresent[person]);
    CHECK_STR_EQ(p.verbForm(lem.verbTable, tp::Tense::Preterite, pe), serPreterite[person]);
  }
  CHECK_STR_EQ(p.verbForm(lem.verbTable, tp::Tense::Imperative, tp::Person::Tu), "s\xC3\xA9");
  CHECK_STR_EQ(p.verbForm(lem.verbTable, tp::Tense::Imperative, tp::Person::Yo), "");
  CHECK_STR_EQ(p.verbForm(lem.verbTable, tp::Tense::NegativeImperative, tp::Person::Tu), "seas");
  CHECK_STR_EQ(p.verbForm(lem.verbTable, tp::kTagInfinitive), "ser");
  CHECK(p.lemma(estar, lem));
  const char* estarPresent[] = {"estoy", "est\xC3\xA1s", "est\xC3\xA1", "estamos", "est\xC3\xA1n"};
  for (uint8_t person = 0; person < tp::kPersonCount; ++person) {
    CHECK_STR_EQ(p.verbForm(lem.verbTable, tp::Tense::Present, static_cast<tp::Person>(person)), estarPresent[person]);
  }
  CHECK(p.lemma(lemmaId(p, "llamarse"), lem));
  CHECK(p.verbInfo(lem.verbTable, vi));
  CHECK((vi.flags & tp::kVerbReflexive) != 0);
  CHECK(p.lemma(agua, lem));
  CHECK_STR_EQ(p.verbForm(lem.verbTable, tp::Tense::Present, tp::Person::Yo), "");  // NONE16

  // Tag helpers.
  constexpr uint16_t imp3s = tp::makeVerbTag(tp::Tense::Imperative, tp::Person::Usted);
  static_assert(imp3s == 0x1062);
  static_assert(tp::tagTense(imp3s | tp::kTagEnclitic) == tp::Tense::Imperative);
  static_assert(tp::tagPerson(imp3s | tp::kTagEnclitic) == tp::Person::Usted);
  static_assert(tp::hasEnclitic(imp3s | tp::kTagEnclitic) && !tp::hasEnclitic(imp3s));
  static_assert(!tp::isVerbTag(tp::kTagGerund) && tp::isNonFiniteTag(tp::kTagGerund));

  // Tokens: byte spans in the stored text, where the em dash is the 2-byte
  // C1 slot U+0097.
  const uint16_t graciasDeNada = sentenceId(p,
                                            "Gracias. \xC2\x97"
                                            "De nada.");
  CHECK(p.sentence(graciasDeNada, sent));
  CHECK_EQ(sent.tokenCount, 2);
  tp::Token tok;
  CHECK(p.token(sent, 1, tok));
  CHECK_EQ(tok.start, 11);
  CHECK_EQ(tok.length, 7);
  CHECK((tok.flags & tp::kTokenMultiword) != 0);
  CHECK((tok.flags & tp::kTokenTarget) != 0);
  CHECK_EQ(tok.lemma, lemmaId(p, "de nada"));
  CHECK_TOKEN(p, sent, 0, "Gracias");
  CHECK_TOKEN(p, sent, 1, "De nada");
  CHECK(p.sentence(sentenceId(p,
                              "\xC2\xBF"
                              "C\xC3\xB3mo te llamas? \xC2\x97Me llamo Luis."),
                   sent));
  CHECK_EQ(sent.tokenCount, 6);
  CHECK_TOKEN(p, sent, 0, "C\xC3\xB3mo");
  CHECK_TOKEN(p, sent, 2, "llamas");
  CHECK_TOKEN(p, sent, 3, "Me");
  CHECK_TOKEN(p, sent, 5, "Luis");
  CHECK(p.token(sent, 2, tok));
  CHECK_EQ(tok.tag, tp::makeVerbTag(tp::Tense::Present, tp::Person::Tu));
  CHECK(p.token(sent, 5, tok));
  CHECK((tok.flags & tp::kTokenName) != 0);
  CHECK(p.sentence(sentenceId(p, "Son 20 pesos."), sent));
  CHECK(p.token(sent, 1, tok));
  CHECK_TOKEN(p, sent, 1, "20");
  CHECK((tok.flags & tp::kTokenNumber) != 0);
  CHECK_EQ(tok.lemma, kNone16);

  // Items: cloze on "soy", with the authored confusable "estoy" always shown.
  const uint16_t yoSoy = sentenceId(p, "Yo soy de M\xC3\xA9xico.");
  const int32_t cloze = findItem(p, ItemKind::Cloze, yoSoy, 1);
  CHECK(cloze >= 0);
  tp::Item it;
  CHECK(p.item(static_cast<uint32_t>(cloze), it));
  CHECK(p.sentence(yoSoy, sent));
  CHECK((sent.flags & tp::kSentenceHasTarget) != 0);
  CHECK_TOKEN(p, sent, 1, "soy");
  CHECK(p.token(sent, 1, tok));
  CHECK_EQ(tok.lemma, ser);
  CHECK_EQ(tok.tag, tp::makeVerbTag(tp::Tense::Present, tp::Person::Yo));
  CHECK_EQ(it.flags & tp::kItemMustShowMask, 1);
  CHECK(it.candidateCount >= 3);
  CHECK_STR_EQ(p.str(p.itemCandidate(it, 0)), "estoy");
  CHECK_EQ(it.lesson, 0);

  // Word order: both authored sentences.
  int wordOrder = 0;
  for (uint32_t i = 0; i < p.itemCount(); ++i) {
    CHECK(p.item(i, it));
    if (it.kind != ItemKind::WordOrder) continue;
    ++wordOrder;
    CHECK(p.sentence(it.a, sent));
    CHECK((sent.flags & tp::kSentenceWordOrder) != 0);
    CHECK_EQ(it.candidateCount, 0);
  }
  CHECK_EQ(wordOrder, 2);
  CHECK(findItem(p, ItemKind::WordOrder, sentenceId(p, "T\xC3\xBA eres mi amigo."), 0) >= 0);
  CHECK(findItem(p, ItemKind::WordOrder, sentenceId(p, "Ana come tacos."), 0) >= 0);

  // Conjugation: ser pres.1s, whose distractors are other forms of ser.
  const int32_t conj = findItem(p, ItemKind::Conjugation, ser, tp::makeVerbTag(tp::Tense::Present, tp::Person::Yo));
  CHECK(conj >= 0);
  CHECK(p.item(static_cast<uint32_t>(conj), it));
  CHECK(it.candidateCount >= 3);
  for (uint8_t n = 0; n < it.candidateCount; ++n) {
    CHECK(std::strcmp(p.str(p.itemCandidate(it, n)), "soy") != 0);
  }
  CHECK(findItem(p, ItemKind::Conjugation, lemmaId(p, "hablar"), tp::makeVerbTag(tp::Tense::Present, tp::Person::Yo)) >=
        0);

  // Vocabulary: produce is gated by recognise; gender items exist for nouns.
  int produce = 0;
  for (uint32_t i = 0; i < p.itemCount(); ++i) {
    CHECK(p.item(i, it));
    if (it.kind != ItemKind::VocabProduce) continue;
    ++produce;
    const int32_t rec = findItem(p, ItemKind::VocabRecognise, it.a, 0);
    CHECK(rec >= 0);
    CHECK_EQ(p.prerequisiteOf(i), rec);
    CHECK_EQ(p.kindAt(static_cast<uint32_t>(rec)), ItemKind::VocabRecognise);
    CHECK_EQ(p.prerequisiteOf(static_cast<uint32_t>(rec)), -1);
  }
  CHECK(produce >= 20);
  const int32_t aguaGender = findItem(p, ItemKind::Gender, agua, 0);
  CHECK(aguaGender >= 0);
  CHECK_EQ(p.prerequisiteOf(static_cast<uint32_t>(aguaGender)), findItem(p, ItemKind::VocabRecognise, agua, 0));

  // Frequency-deck and phrasebook items come after the lessons.
  const int32_t diaRec = findItem(p, ItemKind::VocabRecognise,
                                  lemmaId(p,
                                          "d\xC3\xAD"
                                          "a"),
                                  0);
  CHECK(diaRec >= 0);
  CHECK(p.item(static_cast<uint32_t>(diaRec), it));
  CHECK((it.flags & tp::kItemFrequencyDeck) != 0);
  CHECK_EQ(p.lessonAt(static_cast<uint32_t>(diaRec)), kNone16);
  int phrases = 0;
  for (uint32_t i = 0; i < p.itemCount(); ++i) {
    CHECK(p.item(i, it));
    if (it.kind != ItemKind::Phrase) continue;
    ++phrases;
    CHECK((it.flags & tp::kItemPhrasebook) != 0);
    CHECK_EQ(it.lesson, kNone16);
    CHECK(it.candidateCount >= 3);
    tp::PhraseEntry entry;
    CHECK(p.phraseEntry(it.b, entry));
    CHECK_EQ(entry.sentence, it.a);
  }
  CHECK_EQ(phrases, 4);

  // ItemCatalog: uids round-trip; unknown uids are -1.
  for (uint32_t i = 0; i < p.itemCount(); ++i) {
    CHECK_EQ(p.indexOfUid(p.uidAt(i)), static_cast<int32_t>(i));
  }
  CHECK_EQ(p.indexOfUid(0), -1);
  CHECK_EQ(p.indexOfUid(999999), -1);
  CHECK_EQ(p.indexOfUid(0xFFFFFFFFu), -1);

  // Course structure.
  CHECK_EQ(p.count(Section::Unit), 1u);
  tp::Unit unit;
  CHECK(p.unit(0, unit));
  CHECK_STR_EQ(p.str(unit.title),
               "Saludos y cortes\xC3\xAD"
               "a");
  CHECK_STR_EQ(p.str(unit.goals), "Greet people at any time of day.\nChoose between t\xC3\xBA and usted.");
  CHECK_EQ(unit.number, 1);
  CHECK_EQ(unit.lessonCount, 2);
  CHECK_EQ(p.count(Section::Less), 2u);
  tp::Lesson lesson;
  CHECK(p.lesson(0, lesson));
  CHECK_STR_EQ(p.str(lesson.title), "\xC2\xA1Hola!");
  CHECK_EQ(lesson.number, 1);
  CHECK_EQ(lesson.newCount, 11);
  CHECK_EQ(lesson.flags & tp::kLessonReviewed, 0);
  for (uint16_t k = 0; k < lesson.newCount; ++k) {
    CHECK_EQ(p.kindAt(lesson.firstItem + k), ItemKind::VocabRecognise);
    CHECK_EQ(p.lessonAt(lesson.firstItem + k), 0);
  }
  tp::Story story;
  CHECK(p.story(lesson.dialogue, story));
  CHECK_STR_EQ(p.str(story.title), "En la calle");
  CHECK_EQ(story.kind, tp::StoryKind::Dialogue);
  CHECK_EQ(story.lesson, 0);
  tp::StoryLine line;
  CHECK(p.storyLine(story, 0, line));
  CHECK_STR_EQ(p.str(line.speaker), "Ana");
  CHECK(p.sentence(line.sentence, sent));
  CHECK_EQ(sent.source, tp::SentenceSource::Dialogue);

  // The reading, with its two questions.
  CHECK_EQ(p.count(Section::Stor), 3u);
  bool foundReading = false;
  for (uint16_t s = 0; s < p.count(Section::Stor); ++s) {
    CHECK(p.story(s, story));
    if (story.kind != tp::StoryKind::Reading) continue;
    foundReading = true;
    CHECK_STR_EQ(p.str(story.title), "Luis en M\xC3\xA9xico");
    CHECK_EQ(story.lesson, 1);
    CHECK_EQ(story.lineCount, 3);
    CHECK(p.storyLine(story, 0, line));
    CHECK_EQ(line.speaker, 0u);  // narration
    CHECK_EQ(line.flags & tp::kLineNewParagraph, 0);
    CHECK(p.storyLine(story, 1, line));
    CHECK((line.flags & tp::kLineNewParagraph) != 0);
    CHECK(p.sentence(line.sentence, sent));
    CHECK_STR_EQ(p.str(sent.es), "Come tacos con su amigo.");
    CHECK_EQ(sent.source, tp::SentenceSource::Story);
    CHECK_EQ(story.questionCount, 2);
    tp::Question q;
    CHECK(p.question(story, 0, q));
    CHECK_STR_EQ(p.str(q.text),
                 "\xC2\xBF"
                 "D\xC3\xB3nde vive Luis?");
    CHECK((q.flags & tp::kQuestionSpanish) != 0);
    CHECK_EQ(q.optionCount, 3);
    CHECK_EQ(q.answer, 0);
    CHECK_STR_EQ(p.str(q.options[0]), "En M\xC3\xA9xico");
    CHECK_EQ(q.options[3], 0u);
    CHECK(p.question(story, 1, q));
    CHECK_STR_EQ(p.str(q.text), "What does Luis eat?");
    CHECK_EQ(q.flags & tp::kQuestionSpanish, 0);
    CHECK_EQ(q.optionCount, 4);
    CHECK_STR_EQ(p.str(q.options[3]), "bread");
  }
  CHECK(foundReading);

  // Phrasebook.
  CHECK_EQ(p.count(Section::Phrs), 1u);
  tp::PhraseCategory cat;
  CHECK(p.phraseCategory(0, cat));
  CHECK_STR_EQ(p.str(cat.title), "Lo b\xC3\xA1sico");
  CHECK_STR_EQ(p.str(cat.titleEn), "The basics");
  CHECK_EQ(cat.entryCount, 4);
  tp::PhraseEntry entry;
  CHECK(p.phraseEntry(cat, 1, entry));
  CHECK_EQ(entry.category, 0);
  CHECK(p.str(entry.pron)[0] != '\0');
  CHECK(p.sentence(entry.sentence, sent));
  CHECK_STR_EQ(p.str(sent.es), "\xC2\xBFMande?");
  CHECK_STR_EQ(p.str(sent.en), "Pardon?");
  CHECK_EQ(sent.source, tp::SentenceSource::Phrasebook);
  CHECK_EQ(sent.lesson, kNone16);

  // Confusables.
  CHECK_EQ(p.count(Section::Conf), 2u);
  tp::ConfusableSet conf;
  CHECK(p.confusable(0, conf));
  CHECK_STR_EQ(p.str(conf.name), "ser / estar");
  CHECK_EQ(conf.memberCount, 2);
  CHECK_EQ(conf.members[0], ser);
  CHECK_EQ(conf.members[1], estar);
  for (int k = 2; k < 6; ++k) CHECK_EQ(conf.members[k], kNone16);
  for (const char* w : {"ser", "estar", "por", "para"}) {
    CHECK(p.lemma(lemmaId(p, w), lem));
    CHECK((lem.flags & tp::kLemmaConfusable) != 0);
  }

  // Notes: the grammar note uses every span style.
  tp::Note note;
  CHECK(p.note(0, note));
  CHECK_STR_EQ(p.str(note.title), "t\xC3\xBA and usted");
  CHECK_EQ(note.kind, tp::NoteKind::Grammar);
  CHECK_EQ(note.lesson, 0);
  unsigned styles = 0;
  for (uint16_t k = 0; k < note.spanCount; ++k) {
    tp::NoteSpan span;
    CHECK(p.noteSpan(note, k, span));
    styles |= 1u << static_cast<unsigned>(span.style);
    const char* text = p.str(span.text);
    const bool isBreak = span.style == tp::SpanStyle::Paragraph || span.style == tp::SpanStyle::Bullet;
    CHECK_EQ(isBreak, span.text == 0);
    if (std::strcmp(text, "vosotros") == 0) CHECK_EQ(span.style, tp::SpanStyle::NotMexican);
    if (std::strcmp(text, "oos-TEH-dehs") == 0) CHECK_EQ(span.style, tp::SpanStyle::Respelling);
    if (std::strcmp(text, "est\xC3\xA1") == 0) CHECK_EQ(span.style, tp::SpanStyle::SpanishStrong);
    if (std::strcmp(text, "strangers") == 0) CHECK_EQ(span.style, tp::SpanStyle::Strong);
    if (std::strcmp(text, "t\xC3\xBA") == 0) CHECK_EQ(span.style, tp::SpanStyle::Spanish);
  }
  CHECK_EQ(styles, 0xFFu);  // TEXT .. BULLET
  tp::NoteSpan first;
  CHECK(p.noteSpan(note, 0, first));
  CHECK_EQ(first.style, tp::SpanStyle::Text);
  CHECK_STR_EQ(p.str(first.text), "Use ");
  CHECK(p.note(1, note));
  CHECK_EQ(note.kind, tp::NoteKind::Culture);
  CHECK_EQ(note.lesson, 1);
}

// ---- Corruption -------------------------------------------------------------

std::vector<uint8_t> gScratch;
Pack gScratchPack;

uint8_t* fresh(const std::vector<uint8_t>& orig) {
  gScratch = orig;
  return gScratch.data();
}

PackStatus reopen(uint32_t size) { return gScratchPack.open(gScratch.data(), size); }
PackStatus reopen() { return reopen(static_cast<uint32_t>(gScratch.size())); }

void put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, sizeof v); }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof v); }
uint32_t get32(const uint8_t* p) {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof v);
  return v;
}

// Byte offset of the directory entry for `tag`.
uint32_t dirEntry(const uint8_t* d, const char (&tag)[5]) {
  tp::Header h;
  std::memcpy(&h, d, sizeof h);
  for (uint32_t i = 0; i < h.sectionCount; ++i) {
    const uint32_t at = h.directoryOffset + i * sizeof(tp::DirEntry);
    if (get32(d + at) == tp::makeTag(tag)) return at;
  }
  std::printf("  no directory entry %s\n", tag);
  ++tinta_test::failures();
  return 0;
}

void checkClosed(const Pack& p) {
  CHECK(!p.isOpen());
  CHECK(!p.verifyCrc());
  CHECK_EQ(p.itemCount(), 0u);
  CHECK_EQ(p.count(Section::Lemm), 0u);
  CHECK_STR_EQ(p.str(1), "");
  CHECK_STR_EQ(p.locale(), "");
  tp::Lemma lem;
  CHECK(!p.lemma(0, lem));
  CHECK_EQ(p.findLemmaPrefix("").count, 0u);
  CHECK_EQ(p.indexOfUid(1), -1);
  CHECK_STR_EQ(p.verbForm(0, tp::Tense::Present, tp::Person::Yo), "");
}

void checkCorruption(const std::vector<uint8_t>& orig) {
  const auto size = static_cast<uint32_t>(orig.size());
  uint8_t* d = nullptr;

  // Truncation.
  fresh(orig);
  CHECK_EQ(gScratchPack.open(nullptr, size), PackStatus::TooSmall);
  for (uint32_t n : {0u, 1u, 47u}) CHECK_EQ(reopen(n), PackStatus::TooSmall);
  for (uint32_t n : {48u, 100u, size / 2, size - 4, size - 1}) CHECK_EQ(reopen(n), PackStatus::BadSize);
  checkClosed(gScratchPack);
  // ... with the header's size patched to match: the directory or the last
  // section (STRS) no longer fits.
  d = fresh(orig);
  put32(d + 16, 100);
  CHECK_EQ(reopen(100), PackStatus::BadDirectory);
  const uint32_t strs = dirEntry(d, "STRS");
  const uint32_t strsEnd = get32(d + strs + 4) + get32(d + strs + 8);
  const uint32_t cut = (strsEnd - 1) & ~3u;
  put32(d + 16, cut);
  CHECK_EQ(reopen(cut), PackStatus::BadSection);

  // Header.
  d = fresh(orig);
  d[0] = 'X';
  CHECK_EQ(reopen(), PackStatus::BadMagic);
  d = fresh(orig);
  put16(d + 4, 2);
  CHECK_EQ(reopen(), PackStatus::BadVersion);
  put16(d + 4, 0);
  CHECK_EQ(reopen(), PackStatus::BadVersion);
  d = fresh(orig);
  put32(d + 16, size + 4);
  CHECK_EQ(reopen(), PackStatus::BadSize);
  put32(d + 16, size - 4);
  CHECK_EQ(reopen(), PackStatus::BadSize);
  d = fresh(orig);
  put16(d + 38, 40);  // headerSize
  CHECK_EQ(reopen(), PackStatus::BadSize);
  checkClosed(gScratchPack);

  // Directory.
  d = fresh(orig);
  put32(d + 32, 50);
  CHECK_EQ(reopen(), PackStatus::BadDirectory);
  put32(d + 32, size);
  CHECK_EQ(reopen(), PackStatus::BadDirectory);
  put32(d + 32, 0xFFFFFFF0u);
  CHECK_EQ(reopen(), PackStatus::BadDirectory);
  put32(d + 32, 40);  // overlaps the header
  CHECK_EQ(reopen(), PackStatus::BadDirectory);
  d = fresh(orig);
  put16(d + 36, 0xFFFF);
  CHECK_EQ(reopen(), PackStatus::BadDirectory);
  d = fresh(orig);
  put32(d + dirEntry(d, "LKEY"), tp::makeTag("LEMM"));  // LEMM listed twice
  CHECK_EQ(reopen(), PackStatus::BadDirectory);

  // A section outside the pack or misaligned.
  const uint32_t lemm = dirEntry(orig.data(), "LEMM");
  d = fresh(orig);
  put32(d + lemm + 4, size);
  CHECK_EQ(reopen(), PackStatus::BadSection);
  d = fresh(orig);
  put32(d + lemm + 8, size);
  CHECK_EQ(reopen(), PackStatus::BadSection);
  d = fresh(orig);
  put32(d + lemm + 8, 0xFFFFFFF0u);  // offset + size wraps 32 bits
  CHECK_EQ(reopen(), PackStatus::BadSection);
  d = fresh(orig);
  put32(d + lemm + 4, 0xFFFFFFFCu);
  CHECK_EQ(reopen(), PackStatus::BadSection);
  d = fresh(orig);
  put32(d + lemm + 4, get32(d + lemm + 4) + 2);
  CHECK_EQ(reopen(), PackStatus::BadSection);
  // ... including one this reader does not know.
  d = fresh(orig);
  put32(d + dirEntry(d, "CONF"), tp::makeTag("ZZZZ"));
  put32(d + dirEntry(d, "ZZZZ") + 4, 2);
  CHECK_EQ(reopen(), PackStatus::BadSection);

  // A required section missing (renamed to an unknown tag, which is ignored).
  for (const char* tag : {"LEMM", "STRS", "LEXS", "CONF"}) {
    d = fresh(orig);
    const char name[5] = {tag[0], tag[1], tag[2], tag[3], 0};
    put32(d + dirEntry(d, name), tp::makeTag("ZZZZ"));
    CHECK_EQ(reopen(), PackStatus::MissingSection);
  }

  // Record geometry: size not count x stride, stride below the record size,
  // records in an empty section.
  const uint32_t lemmCount = get32(orig.data() + lemm + 12);
  const uint32_t lemmSize = get32(orig.data() + lemm + 8);
  d = fresh(orig);
  put32(d + lemm + 12, lemmCount - 1);
  CHECK_EQ(reopen(), PackStatus::BadSection);
  put32(d + lemm + 12, lemmCount * 2);  // stride 24 < 48
  CHECK_EQ(reopen(), PackStatus::BadSection);
  put32(d + lemm + 12, 0);
  CHECK_EQ(reopen(), PackStatus::BadSection);
  d = fresh(orig);
  put32(d + lemm + 8, lemmSize - 48);  // one record short of count
  CHECK_EQ(reopen(), PackStatus::BadSection);
  d = fresh(orig);
  const uint32_t lexs = dirEntry(d, "LEXS");
  put32(d + lexs + 8, get32(d + lexs + 8) - 1);  // odd size for u16 records
  CHECK_EQ(reopen(), PackStatus::BadSection);

  // Strings.
  const uint32_t strsOffset = get32(orig.data() + strs + 4);
  const uint32_t strsSize = get32(orig.data() + strs + 8);
  d = fresh(orig);
  d[strsOffset + strsSize - 1] = 'x';
  CHECK_EQ(reopen(), PackStatus::BadStrings);
  d = fresh(orig);
  d[strsOffset] = 'x';
  CHECK_EQ(reopen(), PackStatus::BadStrings);
  d = fresh(orig);
  put32(d + strs + 8, 0);
  CHECK_EQ(reopen(), PackStatus::BadStrings);
  checkClosed(gScratchPack);

  // CRC: open() does not read it; verifyCrc() catches a flipped bit in the
  // header (contentVersion, the CRC field itself, reserved), in record data
  // and in the string heap.
  const uint32_t lemmData = get32(orig.data() + lemm + 4);
  for (uint32_t at : {8u, 21u, 44u, lemmData + 1, size / 2, strsOffset + strsSize - 2}) {
    d = fresh(orig);
    d[at] ^= 0x01;
    CHECK_EQ(reopen(), PackStatus::Ok);
    CHECK(!gScratchPack.verifyCrc());
  }
  fresh(orig);
  CHECK_EQ(reopen(), PackStatus::Ok);
  CHECK(gScratchPack.verifyCrc());

  // A newer minor version opens.
  d = fresh(orig);
  put16(d + 6, 9);
  CHECK_EQ(reopen(), PackStatus::Ok);
  CHECK_EQ(gScratchPack.formatMinor(), 9);
  CHECK(!gScratchPack.verifyCrc());
  // A release flag is reported.
  d = fresh(orig);
  put32(d + 40, tp::kHeaderRelease);
  CHECK_EQ(reopen(), PackStatus::Ok);
  CHECK(gScratchPack.isRelease());

  gScratchPack.close();
  checkClosed(gScratchPack);
}

// A pack as a later minor version could write it: LKEY records grown to 12
// bytes and an unknown section appended. Every LKEY record and lookup must
// read as before.
void checkFutureMinor(const std::vector<uint8_t>& orig, const Pack& base) {
  tp::Header h;
  std::memcpy(&h, orig.data(), sizeof h);
  const uint32_t sections = h.sectionCount + 1u;
  std::vector<uint8_t> out(h.directoryOffset + sections * sizeof(tp::DirEntry), 0);
  std::memcpy(out.data(), orig.data(), h.directoryOffset);
  std::vector<tp::DirEntry> dir;
  for (uint32_t i = 0; i < h.sectionCount; ++i) {
    tp::DirEntry e;
    std::memcpy(&e, orig.data() + h.directoryOffset + i * sizeof e, sizeof e);
    while (out.size() % 4 != 0) out.push_back(0);
    tp::DirEntry n = e;
    n.offset = static_cast<uint32_t>(out.size());
    if (e.tag == tp::makeTag("LKEY")) {
      for (uint32_t r = 0; r < e.count; ++r) {
        const uint8_t* rec = orig.data() + e.offset + r * sizeof(tp::LemmaKey);
        out.insert(out.end(), rec, rec + sizeof(tp::LemmaKey));
        out.insert(out.end(), {0xEE, 0xEE, 0xEE, 0xEE});
      }
      n.size = e.count * 12;
    } else {
      out.insert(out.end(), orig.begin() + e.offset, orig.begin() + e.offset + e.size);
    }
    dir.push_back(n);
  }
  while (out.size() % 4 != 0) out.push_back(0);
  dir.push_back(tp::DirEntry{tp::makeTag("ZZZZ"), static_cast<uint32_t>(out.size()), 5, 1});
  out.insert(out.end(), {'h', 'e', 'l', 'l', 'o', 0, 0, 0});
  h.size = static_cast<uint32_t>(out.size());
  h.sectionCount = static_cast<uint16_t>(sections);
  h.formatMinor = 1;
  std::memcpy(out.data(), &h, sizeof h);
  std::memcpy(out.data() + h.directoryOffset, dir.data(), dir.size() * sizeof(tp::DirEntry));

  static Pack grown;
  CHECK_EQ(grown.open(out.data(), static_cast<uint32_t>(out.size())), PackStatus::Ok);
  CHECK_EQ(grown.formatMinor(), 1);
  CHECK_EQ(grown.count(Section::Lkey), base.count(Section::Lkey));
  for (uint32_t i = 0; i < base.count(Section::Lkey); ++i) {
    tp::LemmaKey a, b;
    CHECK(base.lemmaKeyAt(i, a));
    CHECK(grown.lemmaKeyAt(i, b));
    CHECK(sameBytes(a, b));
    const KeyRange ra = base.findLemma(base.str(a.key));
    const KeyRange rb = grown.findLemma(grown.str(b.key));
    CHECK(ra.first == rb.first && ra.count == rb.count);
  }
  CHECK_EQ(grown.findLemmaPrefix("co").count, base.findLemmaPrefix("co").count);
  tp::Lemma a, b;
  CHECK(base.lemma(1, a));
  CHECK(grown.lemma(1, b));
  CHECK(sameBytes(a, b));
}

}  // namespace

int main() {
  const std::vector<uint8_t> packBytes = readFile(kPackPath);
  CHECK(packBytes.size() > 1000);
  gExpect = parseExpect(readLines(kExpectPath));
  CHECK(gExpect.size() > 1000);
  if (packBytes.size() <= 1000 || gExpect.empty()) return tinta_test::result();
  for (const Expect& e : gExpect) {
    if (e.tag == "LEXS") gLexs.push_back(static_cast<uint32_t>(e.get("value")));
    if (e.tag == "DIST") gDist.push_back(static_cast<uint32_t>(e.get("value")));
  }

  checkFold();

  // The same checks at an aligned and at an odd base address: every read is
  // a memcpy, so alignment must not matter.
  static Pack aligned;
  static Pack odd;
  std::vector<uint8_t> oddStorage(packBytes.size() + 1);
  std::memcpy(oddStorage.data() + 1, packBytes.data(), packBytes.size());
  const uint8_t* oddBase = oddStorage.data() + 1;
  CHECK((reinterpret_cast<uintptr_t>(oddBase) & 1) == 1);
  const auto size = static_cast<uint32_t>(packBytes.size());
  CHECK_EQ(aligned.open(packBytes.data(), size), PackStatus::Ok);
  CHECK_EQ(odd.open(oddBase, size), PackStatus::Ok);
  if (!aligned.isOpen() || !odd.isOpen()) return tinta_test::result();
  for (const Pack* p : {&aligned, &odd}) {
    crossCheck(*p);
    checkFixtureFacts(*p);
  }

  checkCorruption(packBytes);
  checkFutureMinor(packBytes, aligned);

  // Reopening replaces the previous pack.
  CHECK_EQ(odd.open(packBytes.data(), size), PackStatus::Ok);
  CHECK(odd.verifyCrc());
  odd.close();
  checkClosed(odd);
  return tinta_test::result();
}

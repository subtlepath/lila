// modules: library reader search pack session lang srs text
//
// M6's core: the mark log (starred words, finished readings), the library
// lookups, story layout for the reader, dictionary search and the sleep
// screen's word. The fixture course (fixtures/sim-fixture.pack) and the
// whole course (fixtures/full.pack, for the readings and the phrasebook);
// see library_test_prepare.sh.

#include "core/library/Library.h"

#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "core/library/MarkLog.h"
#include "core/library/SleepWord.h"
#include "core/pack/Pack.h"
#include "core/reader/Reading.h"
#include "core/search/Search.h"
#include "core/session/Exercise.h"
#include "core/srs/ProgressStore.h"
#include "fakes.h"
#include "host_pack.h"

namespace {

namespace pk = tinta::core::pack;
namespace lib = tinta::core::library;
namespace rd = tinta::core::reader;
namespace sr = tinta::core::search;
using tinta::core::Fsrs;
using tinta::core::Grade;
using tinta::core::ItemKind;
using tinta::core::ItemState;
using tinta::core::ProgressStore;
using tinta_test::MemStore;

using tinta_test::LoadedPack;

LoadedPack gFixture;
LoadedPack gFull;

// ── Mark log ─────────────────────────────────────────────────────────────────

void testMarkJournal() {
  for (bool removal : {false, true}) {
    tinta_test::MemStore store;
    lib::MarkLog marks(store, "read.bin");
    marks.open();
    if (removal) CHECK(marks.add(7));
    const auto before = store.files;
    struct Observer {
      lib::MarkLog& marks;
      bool removal;
      bool accept = false;
      unsigned calls = 0;
    } observer{marks, removal};
    marks.setMutationJournal({&observer,
                              [](void* context, uint32_t key, bool enabled) {
                                auto& observer = *static_cast<Observer*>(context);
                                ++observer.calls;
                                CHECK_EQ(key, 7u);
                                CHECK(enabled == !observer.removal);
                                CHECK(observer.marks.contains(key) == observer.removal);
                                return observer.accept;
                              },
                              nullptr});
    CHECK(!(removal ? marks.remove(7) : marks.add(7)));
    CHECK(marks.journalFailed());
    CHECK(marks.contains(7) == removal);
    CHECK(store.files == before);
    observer.accept = true;
    CHECK(!marks.add(8));
    CHECK_EQ(observer.calls, 1u);
    marks.open();
    CHECK(removal ? marks.remove(7) : marks.add(7));
    const auto calls = observer.calls;
    CHECK(removal ? marks.remove(7) : marks.add(7));
    CHECK_EQ(observer.calls, calls);
  }
  tinta_test::MemStore store;
  lib::MarkLog marks(store, "read.bin");
  struct Recovery {
    tinta_test::MemStore& store;
    unsigned calls = 0;
  } recovery{store};
  marks.setMutationJournal({&recovery, nullptr, [](void* context) {
                              auto& recovery = *static_cast<Recovery*>(context);
                              ++recovery.calls;
                              CHECK_EQ(recovery.store.readCalls, 0);
                              CHECK_EQ(recovery.store.sizeCalls, 0);
                              return false;
                            }});
  marks.open();
  CHECK(marks.journalFailed());
  CHECK(!marks.add(7));
  CHECK(store.files.empty());
  store.present = false;
  marks.open();
  CHECK(!marks.journalFailed());
  CHECK_EQ(recovery.calls, 1u);
}

void testMarkLog() {
  MemStore store;
  {
    lib::MarkLog log(store, "marks.bin");
    log.open();
    CHECK_EQ(log.count(), 0);
    CHECK(log.add(7));
    CHECK(log.add(9));
    CHECK(log.add(7));  // already there
    CHECK_EQ(log.count(), 2);
    CHECK(log.remove(7));
    CHECK(!log.contains(7));
    CHECK(log.contains(9));
    CHECK(log.add(11));
  }
  {
    // What was written comes back, in order.
    lib::MarkLog log(store, "marks.bin");
    log.open();
    CHECK_EQ(log.count(), 2);
    CHECK_EQ(log.at(0), 9);
    CHECK_EQ(log.at(1), 11);
  }
  {
    // A torn last record is skipped; the ones before stand.
    std::vector<uint8_t>& bytes = store.files["marks.bin"];
    bytes.push_back(0x2A);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    lib::MarkLog log(store, "marks.bin");
    log.open();
    CHECK_EQ(log.count(), 2);
  }
  {
    // Many changes: the file is compacted, the set is unchanged.
    MemStore s2;
    lib::MarkLog log(s2, "m.bin");
    log.open();
    for (uint32_t i = 0; i < 200; ++i) {
      log.add(1000 + i % 5);
      log.remove(1000 + (i + 2) % 5);
    }
    const size_t size = s2.files["m.bin"].size();
    CHECK(size < 4 + 8 * 64);
    lib::MarkLog again(s2, "m.bin");
    again.open();
    CHECK_EQ(again.count(), log.count());
    for (uint16_t i = 0; i < log.count(); ++i) CHECK(again.contains(log.at(i)));
  }
  {
    // A guest keeps marks in RAM only.
    MemStore none;
    none.present = false;
    lib::MarkLog log(none, "g.bin");
    log.open();
    CHECK(!log.add(5));
    CHECK(log.contains(5));
    CHECK(none.files.empty());
  }
  {
    // Full: the next add is refused.
    MemStore s3;
    lib::MarkLog log(s3, "f.bin");
    log.open();
    for (uint32_t i = 0; i < lib::MarkLog::kCapacity; ++i) CHECK(log.add(i + 1));
    CHECK(!log.add(99999));
    CHECK(!log.contains(99999));
  }
}

// ── Library ──────────────────────────────────────────────────────────────────

void testLibrary() {
  const pk::Pack& pack = gFull.pack;
  uint16_t stories[64];
  const uint16_t n = lib::readings(pack, stories, 64);
  CHECK(n >= 20);
  for (uint16_t i = 0; i < n; ++i) {
    pk::Story story;
    CHECK(pack.story(stories[i], story));
    CHECK(story.kind == pk::StoryKind::Reading);
    CHECK(lib::storyKey(pack, story) != 0);
    for (uint16_t j = 0; j < i; ++j) {
      pk::Story other;
      pack.story(stories[j], other);
      CHECK(lib::storyKey(pack, story) != lib::storyKey(pack, other));
    }
  }
  std::printf("  %u readings\n", n);

  // Every lemma with items has a recognise item that points back at it.
  int checked = 0;
  for (uint16_t id = 0; id < 2000; id = static_cast<uint16_t>(id + 7)) {
    pk::Lemma lemma;
    if (!pack.lemma(id, lemma)) break;
    const int32_t item = lib::recogniseItem(pack, id);
    if (!(lemma.flags & pk::kLemmaHasItems)) {
      CHECK_EQ(item, -1);
      continue;
    }
    pk::Item record;
    CHECK(item >= 0 && pack.item(static_cast<uint32_t>(item), record));
    CHECK(record.kind == ItemKind::VocabRecognise && record.a == id);
    ++checked;
  }
  CHECK(checked > 20);
  std::printf("  %d lemmas with items checked\n", checked);

  // Every phrasebook category has its phrase items, all of that category.
  const uint32_t categories = pack.count(pk::Section::Phrs);
  CHECK(categories >= 10);
  uint32_t total = 0;
  for (uint16_t c = 0; c < categories; ++c) {
    uint32_t items[96];
    const uint16_t k = lib::categoryItems(pack, c, true, items, 96);
    pk::PhraseCategory category;
    pack.phraseCategory(c, category);
    CHECK(k > 0);
    CHECK(k <= category.entryCount);
    for (uint16_t i = 0; i < k; ++i) {
      pk::Item item;
      pk::PhraseEntry entry;
      CHECK(pack.item(items[i], item) && item.kind == ItemKind::Phrase);
      CHECK(pack.phraseEntry(item.b, entry) && entry.category == c);
    }
    total += k;
  }
  std::printf("  %u categories, %u phrase items\n", categories, total);
}

// ── Reader layout ────────────────────────────────────────────────────────────

void testReading() {
  const pk::Pack& pack = gFull.pack;
  uint16_t stories[64];
  const uint16_t n = lib::readings(pack, stories, 64);
  freeink::ui::BitmapFont font{};
  rd::Fonts fonts{&font, &font};
  uint32_t words = 0;
  uint32_t linked = 0;
  uint32_t unknown = 0;
  for (uint16_t s = 0; s < n; ++s) {
    pk::Story story;
    pack.story(stories[s], story);
    rd::Paragraph paragraphs[32];
    const uint16_t count = rd::paragraphs(pack, story, paragraphs, 32);
    CHECK(count > 0);
    uint16_t lines = 0;
    uint16_t token = 0;
    for (uint16_t p = 0; p < count; ++p) {
      CHECK_EQ(paragraphs[p].firstLine, lines);
      CHECK_EQ(paragraphs[p].firstToken, token);
      lines = static_cast<uint16_t>(lines + paragraphs[p].lineCount);
      tinta::core::text::Span spans[192];
      const uint16_t k = rd::paragraphSpans(pack, story, paragraphs[p], fonts, spans, 192);
      CHECK(k < 192);
      // The spans spell the paragraph's sentences, and the word spans are
      // numbered in order.
      std::string text;
      std::string expect;
      for (uint16_t i = 0; i < k; ++i) {
        text.append(spans[i].text, spans[i].length);
        if (spans[i].token == tinta::core::text::kNoToken) continue;
        CHECK_EQ(spans[i].token, token);
        rd::WordRef ref;
        CHECK(rd::findWord(pack, story, token, ref));
        pk::Sentence sentence;
        pk::Token t;
        CHECK(pack.sentence(ref.sentence, sentence) && pack.token(sentence, ref.token, t));
        const pk::TextSpan word = pack.tokenText(sentence, t);
        CHECK(std::string(word.text, word.length) == std::string(spans[i].text, spans[i].length));
        ++words;
        if (t.lemma != pk::kNone16) {
          ++linked;
        } else if (lib::tokenLemma(pack, sentence, t) == pk::kNone16) {
          ++unknown;
        }
        ++token;
      }
      for (uint16_t l = 0; l < paragraphs[p].lineCount; ++l) {
        pk::StoryLine line;
        pk::Sentence sentence;
        pack.storyLine(story, static_cast<uint16_t>(paragraphs[p].firstLine + l), line);
        pack.sentence(line.sentence, sentence);
        if (l > 0) expect += " ";
        if (line.speaker) expect += std::string(pack.str(line.speaker)) + ": ";
        expect += pack.str(sentence.es);
      }
      CHECK(text == expect);
    }
    CHECK_EQ(lines, story.lineCount);
  }
  rd::WordRef ref;
  pk::Story first;
  pack.story(stories[0], first);
  CHECK(!rd::findWord(pack, first, 60000, ref));
  std::printf("  %u words in the readings: %u linked, %u with no dictionary match\n", words, linked, unknown);
  CHECK(linked * 10 >= words * 8);
}

// ── Search ───────────────────────────────────────────────────────────────────

bool hasLemma(const sr::Search& s, const pk::Pack& pack, const char* es, sr::Match how) {
  for (uint8_t i = 0; i < s.count(); ++i) {
    pk::Lemma lemma;
    if (pack.lemma(s.at(i).lemma, lemma) && std::string(pack.str(lemma.es)) == es && s.at(i).how == how) return true;
  }
  return false;
}

void testSearch() {
  const pk::Pack& pack = gFull.pack;
  sr::Search s;
  CHECK_EQ(s.run(pack, ""), 0);
  CHECK_EQ(s.run(pack, "  "), 0);

  // An inflected form finds its headwords.
  s.run(pack, "fui");
  CHECK(hasLemma(s, pack, "ir", sr::Match::Form));
  CHECK(hasLemma(s, pack, "ser", sr::Match::Form));
  pk::Lemma lemma;
  for (uint8_t i = 0; i < s.count(); ++i) {
    if (s.at(i).how == sr::Match::Form) CHECK(std::string(pack.str(s.at(i).form)) == "fui");
  }

  // Accents and case do not matter; the exact headword comes first.
  s.run(pack, "CAFE");
  CHECK(s.count() > 0);
  CHECK(pack.lemma(s.at(0).lemma, lemma) && std::string(pack.str(lemma.es)) == "café");
  CHECK(s.at(0).how == sr::Match::Headword);

  // A prefix, then English.
  s.run(pack, "aguac");
  CHECK(hasLemma(s, pack, "aguacate", sr::Match::Prefix));
  s.run(pack, "avocado");
  CHECK(hasLemma(s, pack, "aguacate", sr::Match::English));

  // No lemma twice; a short prefix fills the list and says there is more.
  s.run(pack, "c");
  CHECK_EQ(s.count(), sr::Search::kMaxResults);
  CHECK(s.more());
  for (uint8_t i = 0; i < s.count(); ++i) {
    for (uint8_t j = 0; j < i; ++j) CHECK(s.at(i).lemma != s.at(j).lemma);
  }

  // Next letters: under "" every common initial; under "qu" only vowels.
  const uint32_t first = sr::nextLetters(pack, "", false);
  CHECK(first & (1u << ('c' - 'a')));
  CHECK(first & (1u << ('m' - 'a')));
  const uint32_t qu = sr::nextLetters(pack, "qu", false);
  CHECK(qu != 0);
  CHECK(!(qu & (1u << ('z' - 'a'))));
  CHECK(sr::nextLetters(pack, "", true) & (1u << ('h' - 'a')));
  // A space follows "a" (a sus órdenes) and "a sus", never starts a key, and
  // does not follow "agu" (no "agu ...").
  CHECK(sr::nextLetters(pack, "a", false) & (1u << sr::kSpaceBit));
  CHECK(sr::nextLetters(pack, "a sus", false) & (1u << sr::kSpaceBit));
  CHECK(!(first & (1u << sr::kSpaceBit)));
  CHECK(!(sr::nextLetters(pack, "agu", false) & (1u << sr::kSpaceBit)));
  CHECK(sr::nextLetters(pack, "a s", false) & (1u << ('u' - 'a')));
}

// ── Sleep word ───────────────────────────────────────────────────────────────

void testSleepWord() {
  const pk::Pack& pack = gFixture.pack;
  MemStore store;
  Fsrs fsrs;
  std::vector<uint16_t> slots(pack.itemCount());
  ProgressStore progress(store, pack, fsrs);
  progress.open(slots.data(), static_cast<uint32_t>(slots.size()), nullptr, 0);

  // A new learner: a word of the current lesson.
  lib::SleepWord w = lib::pickSleepWord(pack, progress, fsrs, 1000, 0, 0);
  CHECK(!w.learnt);
  pk::Lesson lesson;
  pack.lesson(0, lesson);
  CHECK_EQ(w.item, lesson.firstItem);
  CHECK_EQ(lib::pickSleepWord(pack, progress, fsrs, 1000, 0, 0xFFF0).item, -1);

  // Six recognise items learnt on different days: the pick is one of the five
  // weakest, the same for the same seed, and the strongest never comes.
  std::vector<uint32_t> recognise;
  for (uint32_t i = 0; i < pack.itemCount() && recognise.size() < 6; ++i) {
    pk::Item item;
    pack.item(i, item);
    if (item.kind == ItemKind::VocabRecognise && !tinta::core::session::vulgarItem(pack, item)) recognise.push_back(i);
  }
  CHECK_EQ(recognise.size(), 6);
  for (size_t k = 0; k < recognise.size(); ++k) {
    // The last one is reviewed today: the strongest by far.
    const tinta::core::DayNumber day = static_cast<tinta::core::DayNumber>(k + 1 < recognise.size() ? 990 + k : 1000);
    progress.review(recognise[k], Grade::Good, 1, 1000, day, day * 86400u);
  }
  int seen[64] = {};
  for (uint32_t seed = 0; seed < 20; ++seed) {
    w = lib::pickSleepWord(pack, progress, fsrs, 1000, seed, 0);
    CHECK(w.learnt);
    CHECK(w.item != static_cast<int32_t>(recognise.back()));
    CHECK_EQ(lib::pickSleepWord(pack, progress, fsrs, 1000, seed, 0).item, w.item);
    if (w.item >= 0 && w.item < 64) seen[w.item] = 1;
  }
  int distinct = 0;
  for (int v : seen) distinct += v;
  CHECK_EQ(distinct, lib::kSleepWordPool);

  // Due by tomorrow: the items learnt on day 990..994 fall due within days.
  CHECK(lib::dueBy(progress, 1000, 1001) >= 1);
  CHECK_EQ(lib::dueBy(progress, 1000, 999), 0);
}

}  // namespace

int main() {
  if (!gFixture.load("fixtures/sim-fixture.pack")) {
    std::printf("  missing fixtures/sim-fixture.pack\n");
    return 1;
  }
  testMarkJournal();
  testMarkLog();
  // The readings and the phrasebook need the whole course (run.sh says how
  // to build it).
  if (gFull.load("fixtures/full.pack")) {
    testLibrary();
    testReading();
    testSearch();
  } else {
    std::printf("  no fixtures/full.pack: library, reading and search skipped\n");
  }
  testSleepWord();
  return tinta_test::result();
}

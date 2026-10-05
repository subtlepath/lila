// modules: session pack lang srs
//
// The exercise layer (src/core/session) on the simulator's fixture course
// (fixtures/sim-fixture.pack, from test/session_test_prepare.sh): the
// format picker, the options of every choice item, typed answers, word-order
// tiles, the grade mapping, the vulgar rule and the lesson practice set.

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "check.h"
#include "core/session/Exercise.h"
#include "core/session/Lessons.h"
#include "host_pack.h"

using namespace tinta::core;
using namespace tinta::core::session;
namespace pk = tinta::core::pack;

namespace {

tinta_test::LoadedPack gLoaded;
const pk::Pack& gPack = gLoaded.pack;

bool loadPack() { return gLoaded.load("fixtures/sim-fixture.pack"); }

pk::Item itemAt(uint32_t index) {
  pk::Item item;
  gPack.item(index, item);
  return item;
}

std::string text(const pk::Item& item, Format format, const Option& option) {
  char out[96];
  optionText(gPack, item, format, option, out, sizeof out);
  return out;
}

bool same(const OptionSet& a, const OptionSet& b) {
  if (a.count != b.count || a.answer != b.answer) return false;
  for (uint8_t k = 0; k < a.count; ++k) {
    if (a.options[k].kind != b.options[k].kind || a.options[k].value != b.options[k].value) return false;
  }
  return true;
}

// The first item of `kind` (and lesson, if given).
int32_t firstOf(ItemKind kind, int32_t lesson = -1) {
  for (uint32_t i = 0; i < gPack.itemCount(); ++i) {
    const pk::Item item = itemAt(i);
    if (item.kind == kind && (lesson < 0 || item.lesson == lesson)) return static_cast<int32_t>(i);
  }
  return -1;
}

void testPicker() {
  pk::Item recognise = itemAt(static_cast<uint32_t>(firstOf(ItemKind::VocabRecognise)));
  History fresh;
  CHECK(pickFormat(recognise, 1, fresh, false) == Format::ChooseMeaning);
  CHECK(pickFormat(recognise, 1, fresh, true) == Format::ChooseMeaning);  // recognising is never typed
  History mature;
  mature.isNew = false;
  mature.reps = 6;
  mature.stabilityDays = 30;
  CHECK(pickFormat(recognise, 1, mature, false) == Format::Flashcard);
  // In review, a third of showings are flashcards, decided by uid and reps.
  History review;
  review.isNew = false;
  review.stabilityDays = 5;
  int flashcards = 0;
  for (uint8_t reps = 3; reps < 63; ++reps) {
    review.reps = reps;
    const Format f = pickFormat(recognise, 1234, review, false);
    CHECK(f == Format::Flashcard || f == Format::ChooseMeaning);
    CHECK(pickFormat(recognise, 1234, review, false) == f);
    flashcards += f == Format::Flashcard;
  }
  CHECK(flashcards >= 10 && flashcards <= 30);
  recognise.candidateCount = 0;
  CHECK(pickFormat(recognise, 1, fresh, false) == Format::Flashcard);

  const pk::Item produce = itemAt(static_cast<uint32_t>(firstOf(ItemKind::VocabProduce)));
  CHECK(pickFormat(produce, 1, fresh, false) == Format::ChooseWord);
  CHECK(pickFormat(produce, 1, fresh, true) == Format::TypeWord);
  CHECK(pickFormat(produce, 1, mature, false) == Format::Flashcard);
  CHECK(pickFormat(produce, 1, mature, true) == Format::TypeWord);
  const pk::Item cloze = itemAt(static_cast<uint32_t>(firstOf(ItemKind::Cloze)));
  CHECK(pickFormat(cloze, 1, fresh, false) == Format::ChooseGap);
  CHECK(pickFormat(cloze, 1, fresh, true) == Format::TypeGap);
  const pk::Item conjugation = itemAt(static_cast<uint32_t>(firstOf(ItemKind::Conjugation)));
  CHECK(pickFormat(conjugation, 1, fresh, false) == Format::ChooseForm);
  CHECK(pickFormat(conjugation, 1, fresh, true) == Format::TypeForm);
  CHECK(pickFormat(itemAt(static_cast<uint32_t>(firstOf(ItemKind::Gender))), 1, fresh, true) == Format::ChooseArticle);
  CHECK(pickFormat(itemAt(static_cast<uint32_t>(firstOf(ItemKind::WordOrder))), 1, fresh, true) ==
        Format::BuildSentence);
  CHECK(pickFormat(itemAt(static_cast<uint32_t>(firstOf(ItemKind::Phrase))), 1, fresh, true) == Format::Flashcard);
  CHECK(isChoice(Format::ChooseGap) && !isChoice(Format::TypeGap) && isTyped(Format::TypeForm));
}

Format choiceFormat(ItemKind kind) {
  switch (kind) {
    case ItemKind::VocabRecognise:
      return Format::ChooseMeaning;
    case ItemKind::VocabProduce:
      return Format::ChooseWord;
    case ItemKind::Cloze:
      return Format::ChooseGap;
    case ItemKind::Conjugation:
      return Format::ChooseForm;
    case ItemKind::Gender:
      return Format::ChooseArticle;
    default:
      return Format::Flashcard;
  }
}

// Every choice item of the course: 2-4 options, distinct as shown, the answer
// right, authored partners present, the same set for the same repetition.
void testOptions() {
  int items = 0;
  int varied = 0;
  int withPartners = 0;
  for (uint32_t i = 0; i < gPack.itemCount(); ++i) {
    const pk::Item item = itemAt(i);
    const Format format = choiceFormat(item.kind);
    if (format == Format::Flashcard) continue;
    OptionSet a;
    if (!pickOptions(gPack, item, format, item.uid, 0, false, a)) {
      // Only a noun with no single gender, or an item with no candidates.
      CHECK(item.kind == ItemKind::Gender || item.candidateCount == 0);
      continue;
    }
    ++items;
    CHECK(a.count >= 2 && a.count <= OptionSet::kMax);
    CHECK(a.answer < a.count);
    std::set<std::string> seen;
    for (uint8_t k = 0; k < a.count; ++k) {
      const std::string t = text(item, format, a.options[k]);
      CHECK(!t.empty());
      CHECK(seen.insert(t).second);
    }
    // The right option is the item's own answer.
    const Option right = a.options[a.answer];
    switch (item.kind) {
      case ItemKind::VocabRecognise:
      case ItemKind::VocabProduce:
        CHECK(right.kind == OptionKind::Lemma && right.value == item.a);
        break;
      case ItemKind::Cloze:
        CHECK(right.kind == OptionKind::Token && right.value == item.b);
        break;
      case ItemKind::Conjugation:
        CHECK(right.kind == OptionKind::Form && right.value == item.b);
        break;
      case ItemKind::Gender:
        CHECK(right.kind == OptionKind::Article);
        CHECK(a.options[0].value == 1 && a.options[1].value == 2);
        break;
      default:
        break;
    }
    // Authored partners are always among the options.
    const uint8_t must = item.flags & pk::kItemMustShowMask;
    if (must > 0) {
      ++withPartners;
      for (uint8_t m = 0; m < must && m < item.candidateCount; ++m) {
        const uint32_t candidate = gPack.itemCandidate(item, m);
        bool found = false;
        for (uint8_t k = 0; k < a.count; ++k) found |= a.options[k].value == candidate && k != a.answer;
        CHECK(found);
      }
    }
    OptionSet again;
    CHECK(pickOptions(gPack, item, format, item.uid, 0, false, again));
    CHECK(same(again, a));
    OptionSet later;
    pickOptions(gPack, item, format, item.uid, 5, false, later);
    varied += !same(later, a);
    // Dictionary-only and vulgar lemmas are never offered.
    for (uint8_t k = 0; k < a.count; ++k) {
      if (a.options[k].kind != OptionKind::Lemma) continue;
      pk::Lemma lemma;
      gPack.lemma(static_cast<uint16_t>(a.options[k].value), lemma);
      CHECK(!(lemma.flags & pk::kLemmaDictionaryOnly));
      if (k != a.answer) CHECK(lemma.reg != pk::Register::Vulgar);
    }
  }
  std::printf("  %d choice items; %d show other options at another repetition; %d with authored partners\n", items,
              varied, withPartners);
  CHECK(items > 300);
  CHECK(varied > items / 2);
}

void testArticles() {
  // A noun that takes el though it is feminine (el agua) asks for el.
  for (uint32_t i = 0; i < gPack.itemCount(); ++i) {
    const pk::Item item = itemAt(i);
    if (item.kind != ItemKind::Gender) continue;
    pk::Lemma lemma;
    gPack.lemma(item.a, lemma);
    OptionSet set;
    if (!pickOptions(gPack, item, Format::ChooseArticle, item.uid, 0, false, set)) continue;
    const std::string right = text(item, Format::ChooseArticle, set.options[set.answer]);
    if (std::strcmp(gPack.str(lemma.es), "agua") == 0) CHECK(right == "el");
    if (std::strcmp(gPack.str(lemma.es), "casa") == 0) CHECK(right == "la");
    if (lemma.gender == pk::Gender::Masculine) CHECK(right == "el" || right == "los");
  }
}

void testTyped() {
  char want[96];
  const pk::Item produce = itemAt(static_cast<uint32_t>(firstOf(ItemKind::VocabProduce)));
  typedAnswer(gPack, produce, Format::TypeWord, want, sizeof want);
  pk::Lemma lemma;
  gPack.lemma(produce.a, lemma);
  CHECK(std::strcmp(want, gPack.str(lemma.es)) == 0);
  CHECK(gradeTyped(lang::checkAnswer(want, want, gPack.str(lemma.alt))) == Grade::Good);

  const pk::Item cloze = itemAt(static_cast<uint32_t>(firstOf(ItemKind::Cloze)));
  typedAnswer(gPack, cloze, Format::TypeGap, want, sizeof want);
  CHECK(want[0] != '\0');
  const pk::Item conjugation = itemAt(static_cast<uint32_t>(firstOf(ItemKind::Conjugation)));
  typedAnswer(gPack, conjugation, Format::TypeForm, want, sizeof want);
  CHECK(want[0] != '\0');
  CHECK(gradeTyped(lang::Verdict::Accents) == Grade::Hard);
  CHECK(gradeTyped(lang::Verdict::Typo) == Grade::Hard);
  CHECK(gradeTyped(lang::Verdict::Alternative) == Grade::Good);
  CHECK(gradeTyped(lang::Verdict::Wrong) == Grade::Again);
  CHECK(gradeChoice(true) == Grade::Good && gradeChoice(false) == Grade::Again);
  CHECK(gradeTiles(0) == Grade::Good && gradeTiles(1) == Grade::Hard && gradeTiles(2) == Grade::Again);
}

void testWordOrder() {
  int sentences = 0;
  for (uint32_t i = 0; i < gPack.itemCount(); ++i) {
    const pk::Item item = itemAt(i);
    if (item.kind != ItemKind::WordOrder) continue;
    pk::Sentence sentence;
    CHECK(gPack.sentence(item.a, sentence));
    WordOrder order;
    CHECK(order.begin(gPack, sentence, showingSeed(item.uid, 0)));
    ++sentences;
    CHECK_EQ(order.tileCount(), sentence.tokenCount);
    // Never handed out in order.
    bool inOrder = true;
    for (uint8_t k = 0; k < order.tileCount(); ++k) {
      pk::Token token;
      gPack.token(sentence, k, token);
      const pk::TextSpan word = gPack.tokenText(sentence, token);
      const pk::TextSpan tile = order.tile(k);
      inOrder &= word.length == tile.length && std::memcmp(word.text, tile.text, word.length) == 0;
    }
    CHECK(!inOrder);
    // A wrong pick first, then the right ones.
    uint8_t wrong = 0xFF;
    for (uint8_t k = 0; k < order.tileCount() && wrong == 0xFF; ++k) {
      pk::Token first;
      gPack.token(sentence, 0, first);
      const pk::TextSpan word = gPack.tokenText(sentence, first);
      const pk::TextSpan tile = order.tile(k);
      if (tile.length != word.length || std::memcmp(tile.text, word.text, word.length) != 0) wrong = k;
    }
    CHECK(wrong != 0xFF);
    CHECK(!order.pick(wrong));
    CHECK_EQ(order.mistakes(), 1);
    CHECK_EQ(order.placedCount(), 0);
    for (uint8_t w = 0; w < sentence.tokenCount; ++w) {
      pk::Token token;
      gPack.token(sentence, w, token);
      const pk::TextSpan word = gPack.tokenText(sentence, token);
      bool picked = false;
      for (uint8_t k = 0; k < order.tileCount() && !picked; ++k) {
        const pk::TextSpan tile = order.tile(k);
        if (order.placed(k) || tile.length != word.length || std::memcmp(tile.text, word.text, word.length) != 0)
          continue;
        picked = order.pick(k);
      }
      CHECK(picked);
    }
    CHECK(order.done());
    CHECK_EQ(order.mistakes(), 1);
    CHECK(!order.pick(0));  // nothing left to pick
  }
  CHECK(sentences > 10);
}

void testVulgarAndLessons() {
  int vulgar = 0;
  for (uint32_t i = 0; i < gPack.itemCount(); ++i) vulgar += vulgarItem(gPack, itemAt(i));
  // The fixture's deck word pinche: recognise, produce, its cloze.
  CHECK_EQ(vulgar, 3);

  uint32_t picked[64];
  const uint16_t n = lessonPractice(gPack, 0, false, picked, 64);
  pk::Lesson lesson;
  gPack.lesson(0, lesson);
  CHECK(n > lesson.newCount);
  for (uint16_t k = 0; k < n; ++k) {
    const pk::Item item = itemAt(picked[k]);
    CHECK_EQ(item.lesson, 0);
    if (k < lesson.newCount) {
      CHECK(item.kind == ItemKind::VocabRecognise);
      CHECK_EQ(picked[k], lesson.firstItem + k);
    } else {
      CHECK(item.prereq == pk::kNone16);
    }
  }
  CHECK_EQ(lessonPractice(gPack, 0, false, picked, 3), 3);
  CHECK_EQ(lessonPractice(gPack, 0xFFF0, false, picked, 64), 0);
  std::printf("  lesson 0.1 practice: %u items (%u new words)\n", n, lesson.newCount);
}

// resolveFormat() is pickFormat() unless the item cannot be asked that way;
// then a flashcard. Every item, new and after a few grades.
void testResolve() {
  int fallbacks = 0;
  for (uint32_t i = 0; i < gPack.itemCount(); ++i) {
    const pk::Item item = itemAt(i);
    for (uint8_t reps = 0; reps < 3; ++reps) {
      History h;
      h.reps = reps;
      h.isNew = reps == 0;
      h.stabilityDays = reps == 0 ? 0.0f : 2.0f;
      for (int typing = 0; typing < 2; ++typing) {
        const Format picked = pickFormat(item, item.uid, h, typing != 0);
        const Format resolved = resolveFormat(gPack, item, h, typing != 0, false);
        if (resolved != picked) {
          CHECK(resolved == Format::Flashcard);
          ++fallbacks;
        }
      }
    }
  }
  // A recognise item with too few usable options exists only if the pack has
  // one; the vulgar pinche's cloze is offered no vulgar options, so it still
  // resolves to its picked format.
  ItemState state = ItemState::fresh(1);
  state.reps = 4;
  state.setPhase(Phase::Review);
  state.setStabilityDays(30.0f);
  const History mature = historyOf(state);
  CHECK(!mature.isNew);
  CHECK_EQ(mature.reps, 4);
  CHECK(mature.stabilityDays >= kMatureDays);
  CHECK(historyOf(ItemState::fresh(2)).isNew);
  std::printf("  %d showings fall back to a flashcard\n", fallbacks);
}

}  // namespace

int main() {
  if (!loadPack()) {
    std::printf("  no fixtures/sim-fixture.pack\n");
    return 1;
  }
  testPicker();
  testOptions();
  testArticles();
  testTyped();
  testWordOrder();
  testVulgarAndLessons();
  testResolve();
  return tinta_test::result();
}

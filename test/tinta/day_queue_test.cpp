// modules: srs
//
// Session building and in-session behaviour, session.bin, and a simulated
// 30-day learner on a 300-item catalog.

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "check.h"
#include "core/srs/Bytes.h"
#include "core/srs/DayQueue.h"
#include "core/srs/ProgressStore.h"
#include "fakes.h"

using namespace tinta::core;
using namespace tinta_test;
using Status = ProgressStore::Status;

namespace {

struct Rig {
  Rig(MemStore& s, const ItemCatalog& c, uint16_t capacity = 64)
      : store(s),
        catalog(c),
        slots(c.itemCount()),
        entries(capacity),
        progress(s, c, fsrs),
        queue(progress, c, fsrs, clock, entries.data(), capacity) {
    progress.open(slots.data(), static_cast<uint32_t>(slots.size()), nullptr, 0);
  }

  uint32_t uidNow() const { return catalog.uidAt(static_cast<uint32_t>(queue.current())); }
  ItemState stateOf(uint32_t index) {
    ItemState s;
    progress.load(index, s);
    return s;
  }

  MemStore& store;
  const ItemCatalog& catalog;
  Fsrs fsrs;
  FakeClock clock{2000};
  std::vector<uint16_t> slots;
  std::vector<QueueEntry> entries;
  ProgressStore progress;
  DayQueue queue;
};

uint32_t mix(uint32_t a, uint32_t b) {
  uint32_t h = a * 2654435761u ^ (b + 0x9e3779b9u) * 40503u;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return h;
}

void testNewAndLimits() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(30, 5);  // lessons 1..6
  Rig rig(store, catalog);

  SessionLimits limits;
  limits.unlockedThrough = 2;
  CHECK(rig.queue.build(limits));
  // Only recognise items: produce items wait for their prerequisite.
  CHECK_EQ(rig.queue.size(), 10);
  const QueueCounts c = rig.queue.counts();
  CHECK_EQ(c.newLeft, 10);
  CHECK_EQ(c.dueLeft, 0);
  CHECK_EQ(c.remaining, 10);
  std::set<uint32_t> indices;
  for (uint16_t i = 0; i < rig.queue.size(); ++i) {
    const uint32_t index = rig.queue.entries()[i].index;
    indices.insert(index);
    CHECK(catalog.kindAt(index) == ItemKind::VocabRecognise);
    CHECK(catalog.lessonAt(index) <= 2);
  }
  CHECK_EQ(indices.size(), 10);
  CHECK(rig.queue.currentIsNew());

  // Answer three; a rebuild the same day only offers what is left of the limit.
  for (int i = 0; i < 3; ++i) rig.queue.answer(Grade::Easy, 0, 1000);
  CHECK_EQ(rig.progress.newOn(rig.clock.today()), 3);
  CHECK(rig.queue.build(limits));
  CHECK_EQ(rig.queue.counts().newLeft, 7);

  // Session size caps the queue; the new limit can be lowered.
  limits.sessionSize = 4;
  CHECK(rig.queue.build(limits));
  CHECK_EQ(rig.queue.size(), 4);
  limits.sessionSize = 40;
  limits.newPerDay = 2;
  CHECK(rig.queue.build(limits));
  CHECK_EQ(rig.queue.size(), 0);

  // A suspended unseen item is skipped.
  limits.newPerDay = 10;
  rig.progress.setFlags(10, item_flag::kSuspended, rig.clock.today(), 0);
  CHECK(rig.queue.build(limits));
  for (uint16_t i = 0; i < rig.queue.size(); ++i) CHECK(rig.queue.entries()[i].index != 10);
}

// Excluded items (vulgar words with the setting off) are neither introduced
// nor reviewed, and keep their state for when the setting comes back on.
void testExcluded() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(30, 5);
  Rig rig(store, catalog);
  SessionLimits limits;
  limits.unlockedThrough = 6;
  CHECK(rig.queue.build(limits));
  // Learn the first two new items (Easy graduates them at once).
  const uint32_t first = static_cast<uint32_t>(rig.queue.current());
  rig.queue.answer(Grade::Easy, 0, 1000);
  const uint32_t second = static_cast<uint32_t>(rig.queue.current());
  rig.queue.answer(Grade::Easy, 0, 1000);
  struct Context {
    uint32_t a, b;
  } context{first, second};
  limits.excluded = [](void* c, uint32_t index) {
    const Context& x = *static_cast<Context*>(c);
    return index == x.a || index % 3 == 0;
  };
  limits.excludedContext = &context;
  rig.clock.nextDay(30);
  CHECK(rig.queue.build(limits));
  bool sawSecond = false;
  for (uint16_t i = 0; i < rig.queue.size(); ++i) {
    const uint32_t index = rig.queue.entries()[i].index;
    CHECK(index != first);
    CHECK(index % 3 != 0 || index == second);
    sawSecond |= index == second;
  }
  CHECK(sawSecond || second % 3 == 0);
  const ItemState kept = rig.stateOf(first);
  CHECK(!kept.isNew());
  // Off again: the excluded review is back.
  limits.excluded = nullptr;
  CHECK(rig.queue.build(limits));
  bool sawFirst = false;
  for (uint16_t i = 0; i < rig.queue.size(); ++i) sawFirst |= rig.queue.entries()[i].index == first;
  CHECK(sawFirst);
}

// Starred words (SessionLimits::first) come in first, from a locked lesson
// too and past the day's new limit, once only; a repeat, an index out of
// range, an excluded item or one whose prerequisite is not learnt is skipped.
void testFirst() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(30, 5);  // lessons 1..6
  Rig rig(store, catalog);
  SessionLimits limits;
  limits.unlockedThrough = 1;
  // Recognise items of lemmas 25 (lesson 6) and 20 (lesson 5), a repeat, an
  // index past the end, and lemma 20's produce item (prerequisite unlearnt).
  const uint32_t starred[] = {50, 40, 40, 999, 41};
  limits.first = starred;
  limits.firstCount = 5;
  CHECK(rig.queue.build(limits));
  CHECK_EQ(rig.queue.entries()[0].index, 50);
  CHECK_EQ(rig.queue.entries()[1].index, 40);
  int count50 = 0;
  int count40 = 0;
  for (uint16_t i = 0; i < rig.queue.size(); ++i) {
    const uint32_t index = rig.queue.entries()[i].index;
    count50 += index == 50;
    count40 += index == 40;
    CHECK(index != 41);
    CHECK(index == 50 || index == 40 || catalog.lessonAt(index) <= 1);
  }
  CHECK_EQ(count50, 1);
  CHECK_EQ(count40, 1);
  // They count toward the day's new items: the 2 starred and lesson 1's 5.
  CHECK_EQ(rig.queue.counts().newLeft, 7);

  // No new items allowed today: the starred ones still come.
  limits.newPerDay = 0;
  CHECK(rig.queue.build(limits));
  CHECK_EQ(rig.queue.size(), 2);

  // Excluded: left out.
  limits.excluded = [](void*, uint32_t index) { return index == 50; };
  CHECK(rig.queue.build(limits));
  CHECK_EQ(rig.queue.size(), 1);
  CHECK_EQ(rig.queue.entries()[0].index, 40);
  limits.excluded = nullptr;

  // Once learnt, a starred item is a review like any other, not new again;
  // and with lemma 20 learnt its starred produce item may come in now.
  CHECK(rig.queue.build(limits));
  rig.queue.answer(Grade::Easy, 0, 1000);
  rig.queue.answer(Grade::Easy, 0, 1000);
  CHECK(rig.queue.build(limits));
  CHECK_EQ(rig.queue.size(), 1);
  CHECK_EQ(rig.queue.entries()[0].index, 41);
}

void testPrerequisite() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(3, 5);
  Rig rig(store, catalog);
  SessionLimits limits;

  // Recognise item 0 at Good: stability 2.3 days, below the 3-day gate.
  rig.progress.review(0, Grade::Good, 0, 0, rig.clock.today(), 0);
  rig.progress.review(0, Grade::Good, 0, 0, rig.clock.today(), 0);
  rig.progress.review(0, Grade::Good, 0, 0, rig.clock.today(), 0);
  CHECK(rig.stateOf(0).phaseKind() == Phase::Review);
  CHECK(rig.stateOf(0).stabilityDays() < 3.0f);
  rig.clock.nextDay();
  CHECK(rig.queue.build(limits));
  for (uint16_t i = 0; i < rig.queue.size(); ++i) CHECK(rig.queue.entries()[i].index != 1);

  // Once it is past three days, the produce item comes in.
  ItemState s = rig.stateOf(0);
  rig.clock.setDay(s.dueDay);
  rig.progress.review(0, Grade::Good, 0, 0, rig.clock.today(), 0);
  CHECK(rig.stateOf(0).stabilityDays() >= 3.0f);
  rig.clock.nextDay();
  CHECK(rig.queue.build(limits));
  bool found = false;
  for (uint16_t i = 0; i < rig.queue.size(); ++i) found = found || rig.queue.entries()[i].index == 1;
  CHECK(found);
}

void testDueOrderAndCap() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(40, 40);
  Rig rig(store, catalog);
  const DayNumber start = rig.clock.today();

  // Graduate the 40 recognise items on different days so that they are due
  // with different retrievabilities on one later day.
  for (uint32_t k = 0; k < 40; ++k) {
    const uint32_t index = 2 * k;
    rig.progress.review(index, k % 2 ? Grade::Easy : Grade::Good, 0, 0, static_cast<DayNumber>(start + k % 7), 0);
    if (k % 2 == 0) {
      rig.progress.review(index, Grade::Good, 0, 0, static_cast<DayNumber>(start + k % 7), 0);
      rig.progress.review(index, Grade::Good, 0, 0, static_cast<DayNumber>(start + k % 7), 0);
    }
  }
  // One item left mid-step from an earlier session.
  rig.progress.review(1, Grade::Good, 0, 0, static_cast<DayNumber>(start + 3), 0);
  rig.progress.review(2, Grade::Again, 0, 0, static_cast<DayNumber>(start + 30), 0);

  rig.clock.setDay(static_cast<DayNumber>(start + 30));
  SessionLimits limits;
  limits.newPerDay = 0;
  limits.reviewCap = 25;
  CHECK(rig.queue.build(limits));
  // Item 2 was graded today already (a lapse, in its steps) and item 1 is in
  // its steps: both first, outside the cap. Then the 24 lowest
  // retrievabilities: the cap is 25 and one review was graded today.
  const uint16_t n = rig.queue.size();
  CHECK_EQ(rig.progress.reviewsOn(rig.clock.today()), 1);
  CHECK_EQ(n, 2 + 24);
  const QueueCounts c = rig.queue.counts();
  CHECK_EQ(c.stepsLeft, 2);
  CHECK_EQ(c.dueLeft, 24);
  std::set<uint32_t> steps;
  for (uint16_t i = 0; i < 2; ++i) steps.insert(rig.queue.entries()[i].index);
  CHECK(steps.count(1) && steps.count(2));

  float last = -1;
  std::set<uint32_t> chosen;
  for (uint16_t i = 2; i < n; ++i) {
    const uint32_t index = rig.queue.entries()[i].index;
    const ItemState s = rig.stateOf(index);
    const float r = rig.fsrs.retrievability(s.stabilityDays(), rig.clock.today() - s.lastDay);
    CHECK(r >= last - 1e-4f);
    last = r;
    chosen.insert(index);
  }
  // Nothing left out has a lower retrievability than the last one in.
  for (uint32_t k = 0; k < 40; ++k) {
    const uint32_t index = 2 * k;
    if (chosen.count(index) || index == 2) continue;
    const ItemState s = rig.stateOf(index);
    const float r = rig.fsrs.retrievability(s.stabilityDays(), rig.clock.today() - s.lastDay);
    CHECK(r >= last - 1e-4f);
  }
}

void testSpread() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(4, 4);
  Rig rig(store, catalog);
  // Every item of four words due at once.
  for (uint32_t i = 0; i < 8; ++i) rig.progress.review(i, Grade::Easy, 0, 0, rig.clock.today(), 0);
  ItemState s = rig.stateOf(0);
  rig.clock.setDay(static_cast<DayNumber>(s.dueDay + 5));
  SessionLimits limits;
  CHECK(rig.queue.build(limits));
  CHECK_EQ(rig.queue.size(), 8);
  for (uint16_t i = 1; i < rig.queue.size(); ++i) {
    const uint32_t a = rig.queue.entries()[i - 1].index, b = rig.queue.entries()[i].index;
    CHECK(a / 2 != b / 2);
  }
}

void testSteps() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(20, 20);
  Rig rig(store, catalog);
  SessionLimits limits;
  CHECK(rig.queue.build(limits));
  CHECK_EQ(rig.queue.size(), 10);

  // A new item answered Good comes back after exactly three others...
  const int32_t first = rig.queue.current();
  AnswerResult a = rig.queue.answer(Grade::Good, 0, 1000);
  CHECK(a.status == Status::Stored);
  CHECK(a.inSession);
  CHECK_EQ(rig.queue.entries()[3].index, first);
  CHECK_EQ(rig.queue.entries()[3].flags, kEntryStep);
  for (int i = 0; i < 3; ++i) rig.queue.answer(Grade::Easy, 0, 1000);
  CHECK_EQ(rig.queue.current(), first);
  // ...then after eight...
  a = rig.queue.answer(Grade::Good, 0, 1000);
  CHECK(a.inSession);
  CHECK_EQ(rig.queue.size(), 7);  // only six others left: it goes last
  CHECK_EQ(rig.queue.entries()[6].index, first);
  // ...and graduates on the next Good.
  for (int i = 0; i < 6; ++i) rig.queue.answer(Grade::Easy, 0, 1000);
  CHECK_EQ(rig.queue.current(), first);
  a = rig.queue.answer(Grade::Good, 0, 1000);
  CHECK(!a.inSession);
  CHECK(a.intervalDays >= 1);
  CHECK(rig.queue.empty());
  CHECK_EQ(rig.queue.counts().done, 12);

  // With nothing else left, a failed item returns at once and the session
  // still ends.
  rig.clock.nextDay();
  rig.queue.build(limits);
  int guard = 0;
  while (rig.queue.size() > 1 && guard++ < 100) rig.queue.answer(Grade::Easy, 0, 1000);
  const int32_t last = rig.queue.current();
  a = rig.queue.answer(Grade::Again, 0, 1000);
  CHECK(a.inSession);
  CHECK_EQ(rig.queue.current(), last);
  rig.queue.answer(Grade::Good, 0, 1000);
  rig.queue.answer(Grade::Good, 0, 1000);
  CHECK(rig.queue.empty());
}

void testUndoAndPreview() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(20, 20);
  Rig rig(store, catalog);
  SessionLimits limits;
  rig.queue.build(limits);

  IntervalPreview p[4];
  CHECK(rig.queue.preview(p));
  CHECK(p[0].inSession && p[1].inSession && p[2].inSession);
  CHECK(!p[3].inSession);
  CHECK_EQ(p[3].days, rig.fsrs.interval(rig.fsrs.initial(Grade::Easy).stability));

  CHECK(!rig.queue.canUndo());
  const int32_t first = rig.queue.current();
  const QueueCounts before = rig.queue.counts();
  rig.queue.answer(Grade::Again, 0, 3000);
  CHECK_EQ(rig.queue.counts().newLeft, before.newLeft - 1);
  CHECK_EQ(rig.queue.counts().stepsLeft, 1);
  CHECK(rig.queue.canUndo());
  CHECK(rig.queue.undo());
  CHECK(!rig.queue.canUndo());
  CHECK(!rig.queue.undo());
  CHECK_EQ(rig.queue.current(), first);
  CHECK(rig.queue.currentIsNew());
  const QueueCounts after = rig.queue.counts();
  CHECK_EQ(after.remaining, before.remaining);
  CHECK_EQ(after.newLeft, before.newLeft);
  CHECK_EQ(after.stepsLeft, 0);
  CHECK_EQ(after.done, 0);
  CHECK(!rig.progress.seen(static_cast<uint32_t>(first)));
  CHECK_EQ(rig.progress.newOn(rig.clock.today()), 0);
  // Re-answering after an undo works like the first time.
  rig.queue.answer(Grade::Easy, 0, 1000);
  CHECK(rig.progress.seen(static_cast<uint32_t>(first)));
  const StudyTotals t = rig.queue.takeTotals();
  CHECK_EQ(t.reviews, 1);
  CHECK_EQ(t.correct, 1);
  CHECK_EQ(t.newItems, 1);
  CHECK_EQ(t.milliseconds, 1000u);
  CHECK(!rig.queue.canUndo());  // counted: no longer undoable

  // Preview of a graduated item: Again stays in session, the rest grow.
  ItemState s = rig.stateOf(static_cast<uint32_t>(first));
  rig.clock.setDay(s.dueDay);
  limits.newPerDay = 0;
  rig.queue.build(limits);
  CHECK_EQ(rig.queue.current(), first);
  CHECK(rig.queue.preview(p));
  CHECK(p[0].inSession);
  CHECK(!p[1].inSession && !p[2].inSession && !p[3].inSession);
  CHECK(p[1].days <= p[2].days && p[2].days <= p[3].days);
  CHECK(p[2].days > 1);
}

void testLeechAndSuspend() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(20, 20);
  Rig rig(store, catalog);
  SessionLimits limits;
  limits.newPerDay = 0;
  // Item 0 through five lapses.
  rig.progress.review(0, Grade::Easy, 0, 0, rig.clock.today(), 0);
  for (int i = 0; i < 5; ++i) {
    rig.clock.setDay(rig.stateOf(0).dueDay);
    rig.progress.review(0, Grade::Again, 0, 0, rig.clock.today(), 0);
    rig.progress.review(0, Grade::Easy, 0, 0, rig.clock.today(), 0);
  }
  CHECK_EQ(rig.stateOf(0).lapses, 5);
  rig.progress.review(4, Grade::Easy, 0, 0, rig.clock.today(), 0);
  rig.clock.setDay(static_cast<DayNumber>(rig.stateOf(0).dueDay + 40));
  rig.queue.build(limits);
  CHECK_EQ(rig.queue.size(), 2);
  while (rig.queue.current() != 0) rig.queue.answer(Grade::Good, 0, 1000);
  const AnswerResult a = rig.queue.answer(Grade::Again, 0, 1000);
  CHECK(a.lapse);
  CHECK(a.becameLeech);
  CHECK(rig.stateOf(0).flags & item_flag::kLeech);
  // The learner suspends it: it leaves the session (its step return too).
  CHECK(rig.queue.suspend(0) == Status::Stored);
  for (uint16_t i = 0; i < rig.queue.size(); ++i) CHECK(rig.queue.entries()[i].index != 0);
  CHECK(rig.stateOf(0).suspended());
  CHECK(!rig.queue.canUndo());
  rig.clock.setDay(static_cast<DayNumber>(rig.clock.today() + 400));
  rig.queue.build(limits);
  for (uint16_t i = 0; i < rig.queue.size(); ++i) CHECK(rig.queue.entries()[i].index != 0);
}

void testSessionFile() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(20, 20);
  Rig rig(store, catalog);
  rig.queue.build(SessionLimits());
  rig.queue.answer(Grade::Good, 0, 1000);
  rig.queue.answer(Grade::Again, 0, 2000);
  rig.queue.answer(Grade::Easy, 0, 500);

  uint8_t blob[DayQueue::blobSize(64)];
  CHECK(rig.queue.save(store, blob, sizeof blob));
  CHECK_EQ(store.size(DayQueue::kSessionFile), 32 + 5 * rig.queue.size());
  CHECK(rig.queue.kind() == SessionKind::Today);

  // Resume after "sleep": a new queue on the same day continues exactly.
  std::vector<QueueEntry> entries(64);
  DayQueue resumed(rig.progress, catalog, rig.fsrs, rig.clock, entries.data(), 64);
  CHECK(resumed.load(store, blob, sizeof blob));
  CHECK_EQ(resumed.size(), rig.queue.size());
  for (uint16_t i = 0; i < resumed.size(); ++i) {
    CHECK_EQ(resumed.entries()[i].index, rig.queue.entries()[i].index);
    CHECK_EQ(resumed.entries()[i].flags, rig.queue.entries()[i].flags);
  }
  CHECK_EQ(resumed.counts().done, 3);
  CHECK(!resumed.canUndo());
  const StudyTotals t = resumed.takeTotals();
  CHECK_EQ(t.reviews, 3);
  CHECK_EQ(t.correct, 2);
  CHECK_EQ(t.newItems, 3);
  CHECK_EQ(t.milliseconds, 3500u);

  // Another day: stale.
  rig.clock.nextDay();
  CHECK(!resumed.load(store, blob, sizeof blob));
  CHECK(resumed.empty());
  rig.clock.setDay(static_cast<DayNumber>(rig.clock.today() - 1));

  // Corrupt: refused.
  store.files[DayQueue::kSessionFile][30] ^= 1;
  CHECK(!resumed.load(store, blob, sizeof blob));
  CHECK(resumed.empty());

  // A content update while asleep: retired items drop out, the rest resolve
  // by uid in the new order.
  const uint32_t n = rig.queue.serialize(blob, sizeof blob);
  CHECK(n > 0);
  CHECK_EQ(rig.queue.serialize(blob, n - 1), 0);
  const uint32_t gone = catalog.uidAt(rig.queue.entries()[0].index);
  const FakeCatalog updated = catalog.edited({gone}, {}).shuffled(3);
  std::vector<uint16_t> slots(updated.itemCount());
  ProgressStore progress(store, updated, rig.fsrs);
  progress.open(slots.data(), static_cast<uint32_t>(slots.size()), nullptr, 0);
  DayQueue moved(progress, updated, rig.fsrs, rig.clock, entries.data(), 64);
  CHECK(moved.restore(blob, n));
  CHECK_EQ(moved.size(), rig.queue.size() - 1);
  for (uint16_t i = 0; i < moved.size(); ++i) {
    CHECK_EQ(updated.uidAt(moved.entries()[i].index), catalog.uidAt(rig.queue.entries()[i + 1].index));
  }
}

// Lesson items, frequency-list items and phrases, interleaved in catalog order
// the way a pack can have them.
FakeCatalog mixedCatalog() {
  using K = ItemKind;
  std::vector<FakeCatalog::Item> items = {
      {101, K::VocabRecognise, kNoLesson, 0},  // frequency list
      {102, K::VocabRecognise, kNoLesson, 0},
      {1, K::VocabRecognise, 1, 0},  // lesson 1
      {2, K::VocabProduce, 1, 1},
      {3, K::VocabRecognise, 1, 0},
      {4, K::VocabProduce, 1, 3},
      {301, K::Phrase, kNoLesson, 0},  // phrasebook
      {302, K::Phrase, kNoLesson, 0},
      {5, K::VocabRecognise, 2, 0},  // lesson 2
      {6, K::Cloze, 2, 5},
      {201, K::VocabProduce, kNoLesson, 101},  // frequency list, gated
  };
  for (uint32_t uid = 103; uid <= 112; ++uid) items.push_back({uid, K::VocabRecognise, kNoLesson, 0});
  items.push_back({303, K::Phrase, 3, 0});  // a phrase that sits in a lesson
  return FakeCatalog(std::move(items));
}

std::vector<uint32_t> queuedUids(const Rig& rig) {
  std::vector<uint32_t> uids;
  for (uint16_t i = 0; i < rig.queue.size(); ++i) uids.push_back(rig.catalog.uidAt(rig.queue.entries()[i].index));
  return uids;
}

void testFrequencyAndPhrases() {
  const FakeCatalog catalog = mixedCatalog();
  {
    MemStore store;
    Rig rig(store, catalog);
    SessionLimits limits;
    limits.unlockedThrough = 1;
    CHECK(rig.queue.build(limits));
    // Lesson 1's ungated items first, then the frequency list fills the rest;
    // no phrases, nothing from lesson 2, gated items wait.
    const std::vector<uint32_t> expected = {1, 3, 101, 102, 103, 104, 105, 106, 107, 108};
    CHECK(queuedUids(rig) == expected);

    // Lessons 2 and 3 unlocked: lesson items still come first (none were
    // answered); lesson 3's phrase is not introduced.
    limits.unlockedThrough = 3;
    CHECK(rig.queue.build(limits));
    const std::vector<uint32_t> next = {1, 3, 5, 101, 102, 103, 104, 105, 106, 107};
    CHECK(queuedUids(rig) == next);

    // Lesson items learnt (and gating not yet passed): the list fills the day.
    for (uint32_t uid : {1u, 3u, 5u}) {
      rig.progress.review(static_cast<uint32_t>(catalog.indexOfUid(uid)), Grade::Good, 0, 0,
                          static_cast<DayNumber>(rig.clock.today() - 1), 0);
    }
    CHECK(rig.queue.build(limits));
    const std::vector<uint32_t> list = {101, 102, 103, 104, 105, 106, 107, 108, 109, 110};
    std::vector<uint32_t> got = queuedUids(rig);
    got.erase(std::remove_if(got.begin(), got.end(), [](uint32_t u) { return u < 100; }), got.end());
    CHECK(got == list);
    CHECK_EQ(rig.queue.counts().newLeft, 10);

    // Lesson 1 has nothing left today (its produce items wait for their
    // recognise items), so a limit of one goes to the list.
    limits.newPerDay = 1;
    limits.unlockedThrough = 1;
    CHECK(rig.queue.build(limits));
    CHECK_EQ(rig.queue.counts().newLeft, 1);
    bool listItem = false;
    for (uint16_t i = 0; i < rig.queue.size(); ++i) {
      if (rig.queue.entries()[i].flags & kEntryNew) {
        listItem = catalog.uidAt(rig.queue.entries()[i].index) == 101;
      }
    }
    CHECK(listItem);
  }
  {
    // Phrases already learnt (in the phrasebook) come back as due reviews.
    MemStore store;
    Rig rig(store, catalog);
    const uint32_t phrase = static_cast<uint32_t>(catalog.indexOfUid(301));
    CHECK(rig.progress.review(phrase, Grade::Easy, 0, 0, rig.clock.today(), 0).status == Status::Stored);
    rig.clock.setDay(rig.stateOf(phrase).dueDay);
    SessionLimits limits;
    limits.newPerDay = 0;
    CHECK(rig.queue.build(limits));
    CHECK(queuedUids(rig) == std::vector<uint32_t>{301});
  }
}

void testPractice() {
  const FakeCatalog catalog = mixedCatalog();
  MemStore store;
  Rig rig(store, catalog);
  const auto at = [&](uint32_t uid) { return static_cast<uint32_t>(catalog.indexOfUid(uid)); };
  const DayNumber today = rig.clock.today();
  rig.progress.review(at(1), Grade::Easy, 0, 0, static_cast<DayNumber>(today - 2), 0);  // seen, not due
  rig.progress.setFlags(at(3), item_flag::kSuspended, today, 0);

  // A phrasebook category: phrases (new), a seen item, a suspended one, a
  // repeat and a bad index.
  const uint32_t list[] = {at(301), at(302), at(1), at(3), at(301), 9999, at(5)};
  CHECK(rig.queue.buildPractice(list, 7, 42));
  CHECK(rig.queue.kind() == SessionKind::Practice);
  CHECK_EQ(rig.queue.tag(), 42);
  CHECK(queuedUids(rig) == (std::vector<uint32_t>{301, 302, 1, 5}));
  QueueCounts c = rig.queue.counts();
  CHECK_EQ(c.newLeft, 3);
  CHECK_EQ(c.dueLeft, 1);

  // Same steps, journal and undo as Today.
  AnswerResult a = rig.queue.answer(Grade::Good, 0, 1000);
  CHECK(a.status == Status::Stored);
  CHECK(a.inSession);
  CHECK_EQ(rig.queue.entries()[3].index, at(301));
  CHECK_EQ(rig.progress.newOn(today), 1);
  CHECK(rig.queue.undo());
  CHECK_EQ(rig.progress.newOn(today), 0);
  CHECK(queuedUids(rig) == (std::vector<uint32_t>{301, 302, 1, 5}));
  rig.queue.answer(Grade::Good, 0, 1000);
  rig.queue.answer(Grade::Easy, 0, 1000);  // 302
  const uint32_t journal = rig.progress.journalCount();

  // Resume keeps the kind and tag.
  uint8_t blob[DayQueue::blobSize(64)];
  CHECK(rig.queue.save(store, blob, sizeof blob));
  std::vector<QueueEntry> entries(64);
  DayQueue resumed(rig.progress, catalog, rig.fsrs, rig.clock, entries.data(), 64);
  CHECK(resumed.load(store, blob, sizeof blob));
  CHECK(resumed.kind() == SessionKind::Practice);
  CHECK_EQ(resumed.tag(), 42);
  CHECK_EQ(resumed.size(), rig.queue.size());
  while (!resumed.empty()) resumed.answer(Grade::Good, 0, 1000);
  CHECK(rig.progress.journalCount() > journal);

  // The new items practised count toward the day: Today offers fewer.
  CHECK_EQ(rig.progress.newOn(today), 3);
  SessionLimits limits;
  CHECK(rig.queue.build(limits));
  CHECK(rig.queue.kind() == SessionKind::Today);
  CHECK_EQ(rig.queue.counts().newLeft, 7);

  // An empty list is an empty session.
  CHECK(rig.queue.buildPractice(nullptr, 0));
  CHECK(rig.queue.empty());

  // A version 1 session.bin (no kind or tag) reads back as Today.
  CHECK(rig.queue.buildPractice(list, 7, 9));
  const uint32_t n = rig.queue.serialize(blob, sizeof blob);
  std::vector<uint8_t> v1(blob, blob + 24);
  putU16(v1.data() + 4, 1);
  v1[18] = 0;
  v1.insert(v1.end(), blob + 28, blob + n - 4);
  v1.resize(v1.size() + 4);
  putU32(v1.data() + v1.size() - 4, crc32(v1.data(), v1.size() - 4));
  CHECK(resumed.restore(v1.data(), static_cast<uint32_t>(v1.size())));
  CHECK(resumed.kind() == SessionKind::Today);
  CHECK_EQ(resumed.tag(), 0);
  CHECK_EQ(resumed.size(), rig.queue.size());

  // The card gone: save refuses without touching the store.
  store.dropAtSize(store.sizeCalls);
  store.size(DayQueue::kSessionFile);
  CHECK(!rig.queue.save(store, blob, sizeof blob));
  CHECK_EQ(store.attemptsWhileGone, 0);
}

// --- 30 days ------------------------------------------------------------------

struct DayRecord {
  uint16_t dueAtStart = 0;
  uint16_t graded = 0;
  uint16_t reviews = 0;
  uint16_t introduced = 0;
  uint16_t sessions = 0;
};

struct RunResult {
  std::vector<DayRecord> days;
  std::map<std::string, std::vector<uint8_t>> files;
  uint32_t answers = 0;
  uint32_t suspended = 0;
};

// The learner fails a due item with probability 1 - R, a new or stepping item
// one time in five, and otherwise mostly answers Good. Days 12 and 13 are
// skipped; on day 15 a content update reorders the catalog; on day 20 the
// session is put to sleep halfway and resumed from session.bin.
RunResult simulate(const FakeCatalog& catalog) {
  MemStore store;
  FakeCatalog current = catalog;
  Fsrs fsrs;
  FakeClock clock(1000);
  RunResult out;
  uint32_t n = 0;
  SessionLimits limits;

  std::vector<uint16_t> slots(catalog.itemCount());
  std::vector<QueueEntry> entries(128);
  for (int day = 1; day <= 30; ++day) {
    if (day == 12 || day == 13) {
      clock.nextDay();
      out.days.push_back(DayRecord());
      continue;
    }
    if (day == 15) current = catalog.shuffled(15);
    ProgressStore progress(store, current, fsrs);
    const ProgressStore::OpenResult opened =
        progress.open(slots.data(), static_cast<uint32_t>(slots.size()), nullptr, 0);
    CHECK(opened == ProgressStore::OpenResult::Opened || opened == ProgressStore::OpenResult::Created);
    DayQueue queue(progress, current, fsrs, clock, entries.data(), 128);
    DayRecord rec;
    uint16_t forecast[1];
    progress.forecast(clock.today(), forecast, 1);
    rec.dueAtStart = forecast[0];

    for (int session = 0; session < 20; ++session) {
      CHECK(queue.build(limits));
      if (queue.empty()) break;
      ++rec.sessions;
      int inSession = 0;
      while (!queue.empty()) {
        if (day == 20 && session == 0 && inSession == 15) {
          uint8_t blob[DayQueue::blobSize(128)];
          CHECK(queue.save(store, blob, sizeof blob));
          DayQueue woke(progress, current, fsrs, clock, entries.data(), 128);
          CHECK(woke.load(store, blob, sizeof blob));
          CHECK(woke.save(store, blob, sizeof blob));
          CHECK(queue.load(store, blob, sizeof blob));
        }
        const uint32_t index = static_cast<uint32_t>(queue.current());
        ItemState s;
        progress.load(index, s);
        const bool isNew = s.isNew();
        if (isNew) {
          // Gating holds at the moment of introduction.
          const int32_t p = current.prerequisiteOf(index);
          if (p >= 0) {
            ItemState ps;
            progress.load(static_cast<uint32_t>(p), ps);
            CHECK(ps.stabilityDays() >= DayQueue::kPrerequisiteStabilityDays);
          }
        }
        float pFail = 0.2f;
        if (!isNew && !s.inSteps()) {
          const uint32_t elapsed = clock.today() - s.lastDay;
          pFail = 1.0f - fsrs.retrievability(s.stabilityDays(), elapsed);
        }
        const uint32_t roll = mix(current.uidAt(index), n) % 1000;
        Grade g = Grade::Good;
        if (roll < static_cast<uint32_t>(pFail * 1000.0f)) {
          g = Grade::Again;
        } else if (roll % 10 == 0) {
          g = Grade::Hard;
        } else if (roll % 7 == 0) {
          g = Grade::Easy;
        }
        const AnswerResult a = queue.answer(g, 0, 1500 + roll);
        CHECK(a.status == Status::Stored);
        if (a.becameLeech && queue.suspend(index) == Status::Stored) ++out.suspended;
        ++n;
        ++rec.graded;
        ++inSession;
        clock.tick(6);
      }
    }
    rec.reviews = progress.reviewsOn(clock.today());
    rec.introduced = progress.newOn(clock.today());

    // Nothing due is left behind unless the review cap stopped it, and every
    // seen item has a future due day or is suspended.
    for (uint32_t i = 0; i < current.itemCount(); ++i) {
      if (!progress.seen(i)) continue;
      ItemState s;
      progress.load(i, s);
      if (s.suspended()) continue;
      CHECK(s.dueDay > clock.today() || rec.reviews >= limits.reviewCap);
      CHECK(!s.inSteps());
    }
    CHECK(rec.introduced <= limits.newPerDay);
    CHECK(rec.reviews <= limits.reviewCap);
    out.days.push_back(rec);
    clock.nextDay();
  }

  // Every record is a distinct catalog uid; nothing lost or duplicated; the
  // journal rebuilds the same state.
  {
    ProgressStore progress(store, current, fsrs);
    progress.open(slots.data(), static_cast<uint32_t>(slots.size()), nullptr, 0);
    std::set<uint32_t> uids;
    std::vector<std::vector<uint8_t>> records;
    progress.forEachRecord([&](const ItemState& s, int32_t index) {
      CHECK(index >= 0);
      CHECK(uids.insert(s.uid).second);
      uint8_t b[16];
      s.encode(b);
      records.emplace_back(b, b + 16);
    });
    uint32_t seen = 0;
    for (uint32_t i = 0; i < current.itemCount(); ++i) seen += progress.seen(i) ? 1 : 0;
    CHECK_EQ(seen, uids.size());
    CHECK_EQ(progress.journalCount(), n + out.suspended);
    CHECK(progress.rebuild() == ProgressStore::OpenResult::Rebuilt);
    std::vector<std::vector<uint8_t>> rebuilt;
    progress.forEachRecord([&](const ItemState& s, int32_t) {
      uint8_t b[16];
      s.encode(b);
      rebuilt.emplace_back(b, b + 16);
    });
    CHECK(rebuilt == records);
    store.files.erase(ProgressStore::kItemsFile);  // compared below without it
  }
  out.files = store.files;
  out.answers = n;
  return out;
}

void test30Days() {
  const FakeCatalog catalog = FakeCatalog::vocab(150, 5);
  CHECK_EQ(catalog.itemCount(), 300);
  const RunResult a = simulate(catalog);
  const RunResult b = simulate(catalog);

  std::printf("  day  due  graded  reviews  new  sessions\n");
  uint32_t totalNew = 0, totalReviews = 0;
  for (size_t d = 0; d < a.days.size(); ++d) {
    const DayRecord& r = a.days[d];
    std::printf("  %3zu  %3u  %6u  %7u  %3u  %8u\n", d + 1, r.dueAtStart, r.graded, r.reviews, r.introduced,
                r.sessions);
    totalNew += r.introduced;
    totalReviews += r.reviews;
    CHECK_EQ(r.dueAtStart, b.days[d].dueAtStart);
    CHECK_EQ(r.graded, b.days[d].graded);
    CHECK_EQ(r.introduced, b.days[d].introduced);
  }
  CHECK(a.files == b.files);
  CHECK_EQ(a.answers, b.answers);
  std::printf("  %u answers, %u new items, %u due reviews, %u leeches suspended\n", a.answers, totalNew, totalReviews,
              a.suspended);

  // Sensible shape: day 1 is all new; each study day introduces items until
  // gating and the limit allow; reviews start on day 2; the two days away
  // pile up due items for day 14.
  CHECK_EQ(a.days[0].dueAtStart, 0);
  CHECK_EQ(a.days[0].introduced, 10);
  CHECK_EQ(a.days[0].reviews, 0);
  CHECK(a.days[1].dueAtStart > 0);
  CHECK_EQ(a.days[11].graded, 0);
  CHECK(a.days[13].dueAtStart > a.days[10].dueAtStart);
  CHECK(totalNew >= 250 && totalNew <= 280);
  CHECK(totalReviews > totalNew);
  for (size_t d = 0; d < a.days.size(); ++d) {
    if (d == 11 || d == 12) continue;
    CHECK(a.days[d].introduced >= 1);
  }
}

}  // namespace

int main() {
  testNewAndLimits();
  testExcluded();
  testFirst();
  testPrerequisite();
  testDueOrderAndCap();
  testSpread();
  testSteps();
  testUndoAndPreview();
  testLeechAndSuspend();
  testSessionFile();
  testFrequencyAndPhrases();
  testPractice();
  test30Days();
  return tinta_test::result();
}

// modules: srs
//
// items.bin + reviews.log: persistence, undo, flags, replay, rebuild, retired
// and reordered items, guest mode, and power cuts at every write of a session.

#include <array>
#include <cstdio>
#include <vector>

#include "check.h"
#include "core/srs/Bytes.h"
#include "core/srs/DayQueue.h"
#include "core/srs/ProgressStore.h"
#include "fakes.h"

using namespace tinta::core;
using namespace tinta_test;
using OpenResult = ProgressStore::OpenResult;
using Status = ProgressStore::Status;

namespace {

struct Snapshot {
  std::vector<std::array<uint8_t, 16>> records;  // slot order
  uint32_t journal = 0;
  uint16_t newToday = 0;
  uint16_t reviewsToday = 0;

  bool operator==(const Snapshot& o) const {
    return records == o.records && journal == o.journal && newToday == o.newToday && reviewsToday == o.reviewsToday;
  }
  bool operator!=(const Snapshot& o) const { return !(*this == o); }
};

Snapshot snapshot(ProgressStore& progress, DayNumber day) {
  Snapshot s;
  progress.forEachRecord([&](const ItemState& state, int32_t) {
    std::array<uint8_t, 16> bytes;
    state.encode(bytes.data());
    s.records.push_back(bytes);
  });
  s.journal = progress.journalCount();
  s.newToday = progress.newOn(day);
  s.reviewsToday = progress.reviewsOn(day);
  return s;
}

// A ProgressStore with the buffers it borrows.
struct Rig {
  Rig(MemStore& s, const ItemCatalog& c, uint16_t guestCapacity = 0)
      : store(s), catalog(c), slots(c.itemCount()), guest(guestCapacity), progress(s, c, fsrs) {}

  OpenResult open() {
    return progress.open(slots.data(), static_cast<uint32_t>(slots.size()), guest.empty() ? nullptr : guest.data(),
                         static_cast<uint16_t>(guest.size()));
  }

  MemStore& store;
  const ItemCatalog& catalog;
  Fsrs fsrs;
  std::vector<uint16_t> slots;
  std::vector<ItemState> guest;
  ProgressStore progress;
};

Grade gradeFor(uint32_t uid, uint32_t n) {
  uint32_t h = uid * 2654435761u ^ (n + 1) * 40503u;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  const uint32_t roll = h % 100;
  if (roll < 15) return Grade::Again;
  if (roll < 25) return Grade::Hard;
  if (roll < 85) return Grade::Good;
  return Grade::Easy;
}

// Study until the day has nothing left. Returns the number of grades given.
int studyDay(Rig& rig, FakeClock& clock, const SessionLimits& limits) {
  QueueEntry entries[64];
  DayQueue queue(rig.progress, rig.catalog, rig.fsrs, clock, entries, 64);
  int graded = 0;
  for (int session = 0; session < 10; ++session) {
    CHECK(queue.build(limits));
    if (queue.empty()) break;
    while (!queue.empty()) {
      const uint32_t uid = rig.catalog.uidAt(static_cast<uint32_t>(queue.current()));
      const AnswerResult a = queue.answer(gradeFor(uid, static_cast<uint32_t>(graded)), 0, 2000);
      CHECK(a.status == Status::Stored);
      if (a.status != Status::Stored) return graded;
      ++graded;
      clock.tick(5);
    }
  }
  return graded;
}

void testAuthoritativeRecoveryPrecedesLocalReads() {
  const auto catalog = FakeCatalog::vocab(3, 1);
  MemStore canonical;
  {
    Rig initial(canonical, catalog);
    CHECK(initial.open() == OpenResult::Created);
    CHECK(initial.progress.review(0, Grade::Good, 0, 1234, 10, 1).status == Status::Stored);
  }
  for (unsigned mode = 0; mode < 3; ++mode) {
    MemStore store;
    store.present = mode != 2;
    struct Recovery {
      MemStore& target;
      const MemStore& canonical;
      unsigned calls = 0;
      bool accept = false;
    } recovery{store, canonical, 0, mode == 1};
    Rig reopened(store, catalog);
    reopened.progress.setMutationJournal({&recovery, nullptr, [](void* context) {
                                            auto& recovery = *static_cast<Recovery*>(context);
                                            ++recovery.calls;
                                            CHECK_EQ(recovery.target.sizeCalls, 0);
                                            CHECK_EQ(recovery.target.readCalls, 0);
                                            CHECK_EQ(recovery.target.calls, 0);
                                            if (!recovery.accept) return false;
                                            recovery.target.files = recovery.canonical.files;
                                            return true;
                                          }});
    const auto opened = reopened.open();
    CHECK(reopened.progress.authoritativeRecoveryFailed() == (mode == 0));
    if (mode == 0) {
      CHECK(opened == OpenResult::Failed);
      CHECK(store.files.empty());
      CHECK_EQ(store.sizeCalls, 0);
      CHECK_EQ(store.readCalls, 0);
      CHECK_EQ(store.calls, 0);
      CHECK(reopened.progress.review(0, Grade::Good, 0, 0, 10, 2).status == Status::Failed);
    } else if (mode == 1) {
      CHECK(opened != OpenResult::Failed);
      CHECK_EQ(reopened.progress.journalCount(), 1u);
      CHECK(reopened.progress.seen(0));
    } else {
      CHECK(opened == OpenResult::Guest);
      CHECK(store.files.empty());
    }
    CHECK_EQ(recovery.calls, mode == 2 ? 0u : 1u);
    store.present = false;
    CHECK(reopened.open() == OpenResult::Guest);
    CHECK(!reopened.progress.authoritativeRecoveryFailed());
  }
}

void testMutationJournalPrecedesLocalWrites() {
  for (unsigned operation = 0; operation < 3; ++operation) {
    MemStore store;
    const auto catalog = FakeCatalog::vocab(3, 1);
    Rig rig(store, catalog);
    CHECK(rig.open() == OpenResult::Created);
    if (operation == 1) CHECK(rig.progress.review(0, Grade::Good, 0, 0, 10, 1).status == Status::Stored);
    struct Observer {
      MemStore& store;
      decltype(store.files) original;
      unsigned calls = 0;
      bool accept = false;
      uint32_t expectedMilliseconds = 0;
    } observer{store, store.files};
    observer.expectedMilliseconds = operation == 0 ? 1234 : 0;
    rig.progress.setMutationJournal({&observer, [](void* context, const JournalEntry& entry, const ItemState& before,
                                                   const ItemState& after, uint32_t responseMilliseconds) {
                                       auto& observer = *static_cast<Observer*>(context);
                                       ++observer.calls;
                                       CHECK(observer.store.files == observer.original);
                                       CHECK_EQ(before.uid, after.uid);
                                       CHECK_EQ(responseMilliseconds, observer.expectedMilliseconds);
                                       if (responseMilliseconds == UINT32_MAX) CHECK_EQ(entry.arg, 255u);
                                       return observer.accept;
                                     }});
    Status status;
    if (operation == 0)
      status = rig.progress.review(0, Grade::Good, 0, 1234, 10, 2).status;
    else if (operation == 1)
      status = rig.progress.undo(10, 2);
    else
      status = rig.progress.setFlags(0, item_flag::kStarred, 10, 2);
    CHECK(status == Status::Failed);
    CHECK(rig.progress.failed());
    CHECK_EQ(observer.calls, 1u);
    CHECK(store.files == observer.original);
    observer.accept = true;
    CHECK(rig.progress.review(1, Grade::Good, 0, 0, 10, 3).status == Status::Failed);
    CHECK_EQ(observer.calls, 1u);
    CHECK(rig.open() == OpenResult::Opened);
    observer.expectedMilliseconds = UINT32_MAX;
    CHECK(rig.progress.review(1, Grade::Good, 0, UINT32_MAX, 10, 3).status == Status::Stored);
    CHECK_EQ(observer.calls, 2u);
    CHECK(rig.progress.review(99, Grade::Good, 0, 0, 10, 4).status == Status::Invalid);
    CHECK_EQ(observer.calls, 2u);
    CHECK(rig.progress.rebuild() == OpenResult::Rebuilt);
    CHECK_EQ(observer.calls, 2u);
  }
}
void testCreateAndReopen() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(20, 5);
  FakeClock clock(500);
  Snapshot before;
  {
    Rig rig(store, catalog);
    CHECK(rig.open() == OpenResult::Created);
    CHECK_EQ(store.size(ProgressStore::kItemsFile), ProgressStore::kRecordsOffset);

    ProgressStore::ReviewResult r = rig.progress.review(0, Grade::Good, 3, 1500, 500, 1);
    CHECK(r.status == Status::Stored);
    CHECK(r.before.isNew());
    CHECK(r.after.phaseKind() == Phase::Learning);
    CHECK(r.outcome.inSession);
    CHECK(rig.progress.seen(0));
    CHECK(!rig.progress.seen(1));
    CHECK_EQ(rig.progress.recordCount(), 1);
    CHECK_EQ(rig.progress.newOn(500), 1);
    CHECK_EQ(rig.progress.reviewsOn(500), 0);
    CHECK_EQ(store.size(ProgressStore::kItemsFile), 1024 + 16);
    CHECK_EQ(store.size(ProgressStore::kJournalFile), 12);

    // A second grade updates in place; a new item appends.
    rig.progress.review(0, Grade::Good, 3, 1500, 500, 2);
    CHECK_EQ(store.size(ProgressStore::kItemsFile), 1024 + 16);
    rig.progress.review(4, Grade::Easy, 0, 900, 500, 3);
    CHECK_EQ(store.size(ProgressStore::kItemsFile), 1024 + 32);
    CHECK_EQ(store.size(ProgressStore::kJournalFile), 36);
    CHECK_EQ(rig.progress.journalCount(), 3);
    CHECK_EQ(rig.progress.newOn(500), 2);
    CHECK_EQ(rig.progress.newOn(501), 0);

    // Next day: a graduated item graded again counts as a review.
    ItemState s;
    CHECK(rig.progress.load(4, s));
    rig.progress.review(4, Grade::Good, 0, 900, s.dueDay, 4);
    CHECK_EQ(rig.progress.reviewsOn(s.dueDay), 1);
    CHECK_EQ(rig.progress.newOn(s.dueDay), 0);
    before = snapshot(rig.progress, s.dueDay);
  }
  Rig again(store, catalog);
  CHECK(again.open() == OpenResult::Opened);
  ItemState s;
  CHECK(again.progress.load(4, s));
  CHECK(snapshot(again.progress, s.lastDay) == before);
  CHECK(again.progress.seen(0));
  CHECK(again.progress.seen(4));
  CHECK(!again.progress.hasRecord(2));
  CHECK(again.progress.load(2, s));
  CHECK(s.isNew());
  CHECK_EQ(s.uid, catalog.uidAt(2));

  // Bad arguments are refused without touching the files.
  const int calls = store.calls;
  CHECK(again.progress.review(9999, Grade::Good, 0, 0, 1, 1).status == Status::Invalid);
  CHECK(again.progress.review(1, static_cast<Grade>(0), 0, 0, 1, 1).status == Status::Invalid);
  CHECK(again.progress.review(1, static_cast<Grade>(5), 0, 0, 1, 1).status == Status::Invalid);
  CHECK_EQ(store.calls, calls);

  // Too small a slot table.
  std::vector<uint16_t> small(3);
  ProgressStore tiny(store, catalog, again.fsrs);
  CHECK(tiny.open(small.data(), 3, nullptr, 0) == OpenResult::Failed);
}

void testUndoReviewProof() {
  MemStore store;
  const auto catalog = FakeCatalog::vocab(2, 1);
  Rig rig(store, catalog);
  CHECK(rig.open() != OpenResult::Failed);
  JournalEntry entry;
  entry.uid = 777;
  auto before = ItemState::fresh(777);
  auto after = before;
  CHECK(!rig.progress.loadUndoReview(entry, before, after));
  CHECK_EQ(entry.uid, 777);
  const auto result = rig.progress.review(0, Grade::Good, 2, 1234, 5, 100);
  CHECK(result.status == Status::Stored);
  CHECK(rig.progress.loadUndoReview(entry, before, after));
  CHECK_EQ(entry.uid, catalog.uidAt(0));
  CHECK_EQ(entry.time, 100);
  CHECK_EQ(entry.day, 5);
  CHECK(entry.grade() == Grade::Good);
  CHECK_EQ(entry.format(), 2);
  CHECK(before == result.before);
  CHECK(after == result.after);
  const auto savedEntry = entry;
  store.files["items.bin"][1024 + 8] ^= 1;
  CHECK(!rig.progress.loadUndoReview(entry, before, after));
  CHECK_EQ(entry.uid, savedEntry.uid);
  CHECK_EQ(entry.time, savedEntry.time);
  CHECK(before == result.before);
  CHECK(after == result.after);
  store.files["items.bin"][1024 + 8] ^= 1;
  Rig reopened(store, catalog);
  CHECK(reopened.open() != OpenResult::Failed);
  CHECK(reopened.progress.loadUndoReview(entry, before, after));
  CHECK(before == result.before);
  CHECK(after == result.after);
  CHECK(reopened.progress.setFlags(0, item_flag::kStarred, 5, 101) == Status::Stored);
  CHECK(!reopened.progress.loadUndoReview(entry, before, after));
  CHECK(before == result.before);
  CHECK(after == result.after);
}

void testUndo() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(10, 5);
  Rig rig(store, catalog);
  rig.open();
  CHECK(!rig.progress.canUndo());
  CHECK(rig.progress.undo(10, 0) == Status::Invalid);

  // Undo of a first sight leaves a Phase::New record and resets the counter.
  rig.progress.review(2, Grade::Again, 0, 0, 10, 0);
  CHECK(rig.progress.canUndo());
  ItemState restored;
  CHECK(rig.progress.undo(10, 1, &restored) == Status::Stored);
  CHECK(restored.isNew());
  CHECK(!rig.progress.seen(2));
  CHECK(rig.progress.hasRecord(2));
  CHECK_EQ(rig.progress.newOn(10), 0);
  CHECK(!rig.progress.canUndo());  // one level

  // Undo of a later grade restores the record exactly.
  rig.progress.review(2, Grade::Easy, 0, 0, 10, 2);
  ItemState graduated;
  rig.progress.load(2, graduated);
  rig.progress.review(2, Grade::Again, 0, 0, graduated.dueDay, 3);
  CHECK_EQ(rig.progress.reviewsOn(graduated.dueDay), 1);
  CHECK(rig.progress.undo(graduated.dueDay, 4) == Status::Stored);
  ItemState now;
  rig.progress.load(2, now);
  CHECK(now == graduated);
  CHECK_EQ(rig.progress.reviewsOn(graduated.dueDay), 0);
  CHECK_EQ(rig.progress.journalCount(), 5);

  // The journal replays to the same state, undo markers included.
  const Snapshot live = snapshot(rig.progress, graduated.dueDay);
  CHECK(rig.progress.rebuild() == OpenResult::Rebuilt);
  CHECK(snapshot(rig.progress, graduated.dueDay) == live);
  CHECK(!rig.progress.seen(9));

  // A flag change clears undo.
  rig.progress.review(3, Grade::Good, 0, 0, 10, 5);
  CHECK(rig.progress.canUndo());
  CHECK(rig.progress.setFlags(3, item_flag::kStarred, 10, 6) == Status::Stored);
  CHECK(!rig.progress.canUndo());
}

void testFlags() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(10, 5);
  Rig rig(store, catalog);
  rig.open();
  // Suspending an unseen item creates a new-phase record.
  CHECK(rig.progress.setFlags(6, item_flag::kSuspended, 3, 0) == Status::Stored);
  CHECK(rig.progress.hasRecord(6));
  CHECK(!rig.progress.seen(6));
  ItemState s;
  rig.progress.load(6, s);
  CHECK(s.suspended());
  CHECK(s.isNew());
  CHECK_EQ(rig.progress.newOn(3), 0);
  CHECK(rig.progress.setFlags(6, 0xFFFF, 3, 0) == Status::Stored);
  rig.progress.load(6, s);
  CHECK_EQ(s.flags, item_flag::kAll);
  CHECK(rig.progress.setFlags(99, 0, 3, 0) == Status::Invalid);

  const Snapshot live = snapshot(rig.progress, 3);
  Rig again(store, catalog);
  CHECK(again.open() == OpenResult::Opened);
  CHECK(snapshot(again.progress, 3) == live);
  CHECK(again.progress.hasRecord(6));
  CHECK(!again.progress.seen(6));
  store.files.erase(ProgressStore::kItemsFile);
  Rig rebuilt(store, catalog);
  CHECK(rebuilt.open() == OpenResult::Rebuilt);
  CHECK(snapshot(rebuilt.progress, 3) == live);
}

void testForecastAndTotals() {
  MemStore store;
  const FakeCatalog catalog = FakeCatalog::vocab(10, 5);
  Rig rig(store, catalog);
  rig.open();
  const DayNumber today = 300;
  // Graduated items due on known days, by grading them Easy on chosen days:
  // the Easy interval from new is fixed by the weights.
  const uint16_t easy = rig.fsrs.interval(rig.fsrs.initial(Grade::Easy).stability);
  CHECK(easy > 2 && easy < 14);
  rig.progress.review(0, Grade::Easy, 0, 0, static_cast<DayNumber>(today - easy - 5), 0);  // overdue
  rig.progress.review(1, Grade::Easy, 0, 0, static_cast<DayNumber>(today - easy), 0);      // today
  rig.progress.review(2, Grade::Easy, 0, 0, static_cast<DayNumber>(today - easy + 3), 0);  // +3
  rig.progress.review(3, Grade::Easy, 0, 0, static_cast<DayNumber>(today - easy + 3), 0);  // +3
  rig.progress.review(4, Grade::Easy, 0, 0, static_cast<DayNumber>(today + 20), 0);        // beyond
  rig.progress.review(5, Grade::Easy, 0, 0, static_cast<DayNumber>(today - easy + 1), 0);
  rig.progress.setFlags(5, item_flag::kSuspended, today, 0);  // left out
  rig.progress.review(6, Grade::Good, 0, 0, today, 0);        // in its steps: due today
  rig.progress.setFlags(7, item_flag::kStarred, today, 0);    // unseen with a record: left out

  uint16_t counts[14];
  CHECK(rig.progress.forecast(today, counts, 14));
  CHECK_EQ(counts[0], 3);
  CHECK_EQ(counts[1], 0);
  CHECK_EQ(counts[3], 2);
  uint32_t sum = 0;
  for (uint16_t c : counts) sum += c;
  CHECK_EQ(sum, 5);

  ProgressStore::Totals t;
  CHECK(rig.progress.totals(t));
  CHECK_EQ(t.seen, 7);
  CHECK_EQ(t.inSteps, 1);
  CHECK_EQ(t.suspended, 1);
  CHECK_EQ(t.mature, 0);
  CHECK_EQ(t.leeches, 0);
  CHECK_EQ(t.retired, 0);
}

// History over several days with every kind of record, for the tests below.
void makeHistory(MemStore& store, const FakeCatalog& catalog, FakeClock& clock, int days) {
  Rig rig(store, catalog);
  rig.open();
  SessionLimits limits;
  for (int d = 0; d < days; ++d) {
    studyDay(rig, clock, limits);
    if (d == 1) {
      rig.progress.review(0, Grade::Again, 0, 0, clock.today(), clock.nowSeconds());
      rig.progress.undo(clock.today(), clock.nowSeconds());
      rig.progress.setFlags(3, item_flag::kStarred, clock.today(), clock.nowSeconds());
    }
    clock.nextDay();
  }
}

void testRebuild() {
  const FakeCatalog catalog = FakeCatalog::vocab(60, 5);
  MemStore original;
  FakeClock clock(700);
  makeHistory(original, catalog, clock, 6);
  Snapshot reference;
  {
    Rig rig(original, catalog);
    CHECK(rig.open() == OpenResult::Opened);
    reference = snapshot(rig.progress, clock.today());
    CHECK(reference.records.size() > 20);
    CHECK(reference.journal > 60);
  }

  auto reopenAndCompare = [&](MemStore& store, OpenResult expected, const char* what) {
    Rig rig(store, catalog);
    const OpenResult got = rig.open();
    if (got != expected) std::printf("  %s: open gave %d\n", what, static_cast<int>(got));
    CHECK(got == expected);
    const Snapshot s = snapshot(rig.progress, clock.today());
    if (s != reference) std::printf("  %s: state differs\n", what);
    CHECK(s == reference);
    for (uint32_t i = 0; i < catalog.itemCount(); ++i) {
      CHECK_EQ(rig.progress.seen(i), [&] {
        ItemState st;
        rig.progress.load(i, st);
        return !st.isNew();
      }());
    }
  };

  {  // items.bin missing
    MemStore store = original;
    store.files.erase(ProgressStore::kItemsFile);
    reopenAndCompare(store, OpenResult::Rebuilt, "missing items.bin");
    reopenAndCompare(store, OpenResult::Opened, "after rebuild");
  }
  {  // both header copies destroyed
    MemStore store = original;
    auto& f = store.files[ProgressStore::kItemsFile];
    for (int i = 0; i < 80; ++i) f[i] = f[512 + i] = 0x5A;
    reopenAndCompare(store, OpenResult::Rebuilt, "headers");
  }
  {  // the older header copy destroyed (a torn write of the newer one is in
     // testPowerCuts): the newer one suffices
    MemStore store = original;
    auto& f = store.files[ProgressStore::kItemsFile];
    const uint32_t seqA = getU32(f.data() + 8), seqB = getU32(f.data() + 512 + 8);
    f[(seqA < seqB ? 0 : 512) + 30] ^= 0xFF;
    reopenAndCompare(store, OpenResult::Opened, "older header copy");
  }
  {  // a record fails validation
    MemStore store = original;
    store.files[ProgressStore::kItemsFile][1024 + 16 * 5 + 11] = 0xFF;
    reopenAndCompare(store, OpenResult::Rebuilt, "bad record");
  }
  {  // a uid twice
    MemStore store = original;
    auto& f = store.files[ProgressStore::kItemsFile];
    for (int i = 0; i < 4; ++i) f[1024 + 16 * 7 + i] = f[1024 + 16 * 2 + i];
    reopenAndCompare(store, OpenResult::Rebuilt, "duplicate uid");
  }
  {  // items.bin cut short
    MemStore store = original;
    store.files[ProgressStore::kItemsFile].resize(1024 + 16 * 3);
    reopenAndCompare(store, OpenResult::Rebuilt, "short items.bin");
  }
  {  // the log was lost: keep the state, count from zero
    MemStore store = original;
    store.files.erase(ProgressStore::kJournalFile);
    Rig rig(store, catalog);
    CHECK(rig.open() == OpenResult::Opened);
    Snapshot s = snapshot(rig.progress, clock.today());
    CHECK(s.records == reference.records);
    CHECK_EQ(s.journal, 0);
    CHECK(rig.progress.review(1, Grade::Good, 0, 0, clock.today(), 0).status == Status::Stored);
    CHECK_EQ(store.size(ProgressStore::kJournalFile), 12);
  }
  {  // damage while the log is incomplete: drop the damaged records, keep the
     // rest, and stay consistent on later opens (no rebuild from a short log)
    for (int variant = 0; variant < 2; ++variant) {
      MemStore store = original;
      store.files[ProgressStore::kJournalFile].resize(12 * 10);
      auto& f = store.files[ProgressStore::kItemsFile];
      const uint32_t badUid = getU32(f.data() + 1024 + 16 * 5);
      if (variant == 0) {
        f[1024 + 16 * 5 + 11] = 0xFF;  // record 5 fails validation
      } else {
        f.resize(1024 + 16 * 5 + 7);  // records 5.. cut off
      }
      {
        Rig rig(store, catalog);
        CHECK(rig.open() == OpenResult::Salvaged);
        const Snapshot s = snapshot(rig.progress, clock.today());
        CHECK_EQ(s.journal, 10);
        for (size_t i = 0; i < 5; ++i) CHECK(s.records[i] == reference.records[i]);
        if (variant == 0) {
          CHECK_EQ(s.records.size(), reference.records.size() - 1);
          for (size_t i = 6; i < reference.records.size(); ++i) {
            CHECK(s.records[i - 1] == reference.records[i]);
          }
        } else {
          CHECK_EQ(s.records.size(), 5);
        }
        CHECK(!rig.progress.hasRecord(static_cast<uint32_t>(catalog.indexOfUid(badUid))));
        CHECK(
            rig.progress.review(static_cast<uint32_t>(catalog.indexOfUid(badUid)), Grade::Good, 0, 0, clock.today(), 0)
                .status == Status::Stored);
      }
      Rig rig(store, catalog);
      CHECK(rig.open() == OpenResult::Opened);
      CHECK_EQ(rig.progress.journalCount(), 11);
      CHECK(rig.progress.seen(static_cast<uint32_t>(catalog.indexOfUid(badUid))));
    }
  }
  {  // both headers and the log gone: keep every record that reads back
    MemStore store = original;
    store.files.erase(ProgressStore::kJournalFile);
    auto& f = store.files[ProgressStore::kItemsFile];
    for (int i = 0; i < 80; ++i) f[i] = f[512 + i] = 0;
    {
      Rig rig(store, catalog);
      CHECK(rig.open() == OpenResult::Salvaged);  // today's counters are gone
      const Snapshot s = snapshot(rig.progress, clock.today());
      CHECK(s.records == reference.records);
      CHECK_EQ(s.journal, 0);
    }
    f[1024 + 16 * 2 + 11] = 0xFF;
    for (int i = 0; i < 80; ++i) f[i] = f[512 + i] = 0;
    Rig rig(store, catalog);
    CHECK(rig.open() == OpenResult::Salvaged);
    CHECK_EQ(snapshot(rig.progress, clock.today()).records.size(), reference.records.size() - 1);
  }
  {  // a rebuild cut short at every write starts again on the next open
    MemStore probe = original;
    probe.files.erase(ProgressStore::kItemsFile);
    const int start = probe.calls;
    {
      Rig rig(probe, catalog);
      rig.open();
    }
    const int writes = probe.calls - start;
    CHECK(writes > 20);
    for (int k = 0; k < writes; ++k) {
      MemStore store = original;
      store.files.erase(ProgressStore::kItemsFile);
      store.cutAt(store.calls + k, MemStore::Tear::Prefix);
      {
        Rig rig(store, catalog);
        CHECK(rig.open() == OpenResult::Failed);
      }
      store.powerOn();
      reopenAndCompare(store, OpenResult::Rebuilt, "interrupted rebuild");
    }
  }
}

void testRetiredAndReordered() {
  const FakeCatalog catalog = FakeCatalog::vocab(60, 5);
  MemStore store;
  FakeClock clock(800);
  makeHistory(store, catalog, clock, 5);

  std::map<uint32_t, ItemState> before;
  {
    Rig rig(store, catalog);
    rig.open();
    for (uint32_t i = 0; i < catalog.itemCount(); ++i) {
      if (!rig.progress.hasRecord(i)) continue;
      ItemState s;
      rig.progress.load(i, s);
      before[s.uid] = s;
    }
  }
  CHECK(before.size() > 20);

  // Reordered: every uid keeps its state.
  const FakeCatalog moved = catalog.shuffled(7);
  {
    Rig rig(store, moved);
    CHECK(rig.open() == OpenResult::Opened);
    size_t found = 0;
    for (uint32_t i = 0; i < moved.itemCount(); ++i) {
      ItemState s;
      rig.progress.load(i, s);
      CHECK_EQ(s.uid, moved.uidAt(i));
      auto it = before.find(s.uid);
      if (it == before.end()) {
        CHECK(s.isNew());
        CHECK(!rig.progress.hasRecord(i));
      } else {
        CHECK(s == it->second);
        ++found;
      }
    }
    CHECK_EQ(found, before.size());
    // A session on the new order, then back to the old order.
    studyDay(rig, clock, SessionLimits());
  }

  // Retired items stay on disk, survive a rebuild, and come back if the uid
  // returns. New items added by the update start fresh.
  std::map<uint32_t, ItemState> current;
  std::vector<uint32_t> retire;
  {
    Rig rig(store, catalog);
    CHECK(rig.open() == OpenResult::Opened);
    for (uint32_t i = 0; i < catalog.itemCount(); ++i) {
      ItemState s;
      rig.progress.load(i, s);
      current[s.uid] = s;
      if (rig.progress.seen(i) && retire.size() < 4) retire.push_back(s.uid);
    }
  }
  CHECK_EQ(retire.size(), 4);
  const FakeCatalog trimmed = catalog.edited(retire, {{90001, ItemKind::Phrase, 1, 0}, {90002, ItemKind::Cloze, 1, 0}});
  {
    Rig rig(store, trimmed);
    CHECK(rig.open() == OpenResult::Opened);
    ProgressStore::Totals t;
    CHECK(rig.progress.totals(t));
    CHECK_EQ(t.retired, 4);
    int retiredSeen = 0;
    rig.progress.forEachRecord([&](const ItemState&, int32_t index) {
      if (index < 0) ++retiredSeen;
    });
    CHECK_EQ(retiredSeen, 4);
    CHECK(!rig.progress.hasRecord(static_cast<uint32_t>(trimmed.indexOfUid(90001))));
    const Snapshot trimmedState = snapshot(rig.progress, clock.today());
    CHECK(rig.progress.rebuild() == OpenResult::Rebuilt);
    CHECK(snapshot(rig.progress, clock.today()) == trimmedState);
    // Study under the trimmed catalog, including a new item.
    const uint32_t phrase = static_cast<uint32_t>(trimmed.indexOfUid(90001));
    CHECK(rig.progress.review(phrase, Grade::Good, 0, 0, clock.today(), 0).status == Status::Stored);
    clock.nextDay();
    studyDay(rig, clock, SessionLimits());
  }
  {
    Rig rig(store, catalog);
    CHECK(rig.open() == OpenResult::Opened);
    for (uint32_t uid : retire) {
      ItemState s;
      rig.progress.load(static_cast<uint32_t>(catalog.indexOfUid(uid)), s);
      CHECK(s == current[uid]);
    }
    ProgressStore::Totals t;
    rig.progress.totals(t);
    CHECK_EQ(t.retired, 1);  // the phrase graded under the trimmed catalog
  }
}

void testGuest() {
  MemStore store;
  store.present = false;
  const FakeCatalog catalog = FakeCatalog::vocab(20, 5);
  Rig rig(store, catalog, 4);
  CHECK(rig.open() == OpenResult::Guest);
  CHECK(rig.progress.isGuest());
  for (uint32_t i = 0; i < 4; ++i) {
    CHECK(rig.progress.review(i, Grade::Good, 0, 0, 5, 0).status == Status::Stored);
  }
  CHECK(rig.progress.review(1, Grade::Good, 0, 0, 5, 0).status == Status::Stored);
  ItemState s;
  rig.progress.load(1, s);
  CHECK_EQ(s.reps, 2);
  // Full: graded, outcome reported, not kept.
  const ProgressStore::ReviewResult r = rig.progress.review(7, Grade::Good, 0, 0, 5, 0);
  CHECK(r.status == Status::NotStored);
  CHECK(r.outcome.inSession);
  CHECK(!rig.progress.hasRecord(7));
  CHECK(rig.progress.undo(5, 0) == Status::Stored);  // undoes item 1's second grade
  rig.progress.load(1, s);
  CHECK_EQ(s.reps, 1);
  CHECK_EQ(rig.progress.newOn(5), 4);
  CHECK(store.files.empty());
  CHECK_EQ(store.calls, 0);

  // A whole session works in guest mode.
  FakeClock clock(5);
  Rig session(store, catalog, 64);
  CHECK(session.open() == OpenResult::Guest);
  CHECK(studyDay(session, clock, SessionLimits()) > 10);
  CHECK(store.files.empty());

  // No guest buffer at all: nothing is stored, sessions still run.
  Rig bare(store, catalog, 0);
  CHECK(bare.open() == OpenResult::Guest);
  CHECK(bare.progress.review(0, Grade::Good, 0, 0, 5, 0).status == Status::NotStored);
}

void testJournalTail() {
  const FakeCatalog catalog = FakeCatalog::vocab(20, 5);
  MemStore store;
  FakeClock clock(600);
  makeHistory(store, catalog, clock, 2);
  Snapshot reference;
  {
    Rig rig(store, catalog);
    rig.open();
    reference = snapshot(rig.progress, clock.today());
  }
  // Garbage appended past the last whole record, then a partial record.
  auto& log = store.files[ProgressStore::kJournalFile];
  const size_t clean = log.size();
  for (int i = 0; i < 24; ++i) log.push_back(0xA5);
  for (int i = 0; i < 5; ++i) log.push_back(0x11);
  {
    Rig rig(store, catalog);
    CHECK(rig.open() == OpenResult::Opened);
    CHECK(snapshot(rig.progress, clock.today()) == reference);
    // The garbage is blanked and the next record goes where it was.
    CHECK(rig.progress.review(1, Grade::Good, 0, 0, clock.today(), 0).status == Status::Stored);
    CHECK_EQ(rig.progress.journalCount(), reference.journal + 1);
  }
  CHECK(log[clean + 12] == 0);
  Rig rig(store, catalog);
  CHECK(rig.open() == OpenResult::Opened);
  CHECK_EQ(rig.progress.journalCount(), reference.journal + 1);
  const Snapshot live = snapshot(rig.progress, clock.today());
  CHECK(rig.progress.rebuild() == OpenResult::Rebuilt);
  CHECK(snapshot(rig.progress, clock.today()) == live);
}

// A scripted session: grades, an undo, a suspension. Calls `after` with the
// number of operations completed after each one. Stops at the first failure
// and returns the number completed.
template <class After>
int scriptedSession(Rig& rig, FakeClock& clock, After&& after) {
  QueueEntry entries[64];
  DayQueue queue(rig.progress, rig.catalog, rig.fsrs, clock, entries, 64);
  if (!queue.build(SessionLimits())) return 0;
  int ops = 0;
  for (uint32_t n = 0; !queue.empty() && n < 60; ++n) {
    bool ok;
    if (n == 6 || n == 7) {
      ok = queue.undo();
      if (n == 7) ok = !ok && !queue.canUndo();  // one level: the second undo is refused
      if (n == 7) {
        clock.tick(3);
        continue;
      }
    } else if (n == 11) {
      ok = queue.suspend(static_cast<uint32_t>(queue.current())) == Status::Stored;
    } else {
      const uint32_t uid = rig.catalog.uidAt(static_cast<uint32_t>(queue.current()));
      ok = queue.answer(gradeFor(uid, n), 1, 1800).status == Status::Stored;
    }
    if (!ok) return ops;
    ++ops;
    after(ops);
    clock.tick(4);
  }
  return ops;
}

void testPowerCuts() {
  const FakeCatalog catalog = FakeCatalog::vocab(60, 5);
  MemStore base;
  FakeClock historyClock(900);
  makeHistory(base, catalog, historyClock, 4);
  const DayNumber day = historyClock.today();

  // Reference run: the state after every completed operation.
  std::vector<Snapshot> refs;
  int writesPerSession = 0;
  {
    MemStore store = base;
    Rig rig(store, catalog);
    CHECK(rig.open() == OpenResult::Opened);
    refs.push_back(snapshot(rig.progress, day));
    const int start = store.calls;
    FakeClock clock(day);
    const int ops = scriptedSession(rig, clock, [&](int) { refs.push_back(snapshot(rig.progress, day)); });
    writesPerSession = store.calls - start;
    CHECK(ops > 30);
    CHECK_EQ(writesPerSession, ops * 3);
  }

  const MemStore::Tear tears[] = {MemStore::Tear::Nothing, MemStore::Tear::Prefix, MemStore::Tear::Zeros,
                                  MemStore::Tear::Garbage};
  int cases = 0, lostInFlight = 0;
  for (MemStore::Tear tear : tears) {
    for (int k = 0; k < writesPerSession; ++k) {
      MemStore store = base;
      int completed = 0;
      {
        Rig rig(store, catalog);
        rig.open();
        store.cutAt(store.calls + k, tear);
        FakeClock clock(day);
        completed = scriptedSession(rig, clock, [](int) {});
        CHECK(store.dead());
      }
      store.powerOn();
      Rig rig(store, catalog);
      const OpenResult r = rig.open();
      CHECK(r == OpenResult::Opened || r == OpenResult::Replayed);
      const Snapshot s = snapshot(rig.progress, day);
      const bool same = s == refs[completed];
      const bool next = static_cast<size_t>(completed + 1) < refs.size() && s == refs[completed + 1];
      if (!same && !next) {
        std::printf("  cut at write %d (tear %d): state matches neither %d nor %d ops\n", k, static_cast<int>(tear),
                    completed, completed + 1);
      }
      CHECK(same || next);
      if (same) ++lostInFlight;

      // The reopened store is consistent: the journal rebuilds to it, and it
      // takes further reviews.
      CHECK(rig.progress.rebuild() == OpenResult::Rebuilt);
      CHECK(snapshot(rig.progress, day) == s);
      CHECK(rig.progress.review(0, Grade::Good, 0, 0, day, 0).status == Status::Stored);
      Rig again(store, catalog);
      CHECK(again.open() == OpenResult::Opened);
      ++cases;
    }
  }
  std::printf("  %d power cuts over %d writes per session; in-flight operation lost in %d\n", cases, writesPerSession,
              lostInFlight);

  // The card is pulled mid-session: everything fails cleanly, and with the
  // card back the store holds every completed operation.
  for (int k = 0; k < writesPerSession; k += 7) {
    MemStore store = base;
    int completed = 0;
    {
      Rig rig(store, catalog);
      rig.open();
      store.failFrom(store.calls + k);
      FakeClock clock(day);
      completed = scriptedSession(rig, clock, [](int) {});
      CHECK(rig.progress.failed());
      CHECK(rig.progress.review(1, Grade::Good, 0, 0, day, 0).status == Status::Failed);
    }
    store.powerOn();
    Rig rig(store, catalog);
    CHECK(rig.open() != OpenResult::Failed);
    const Snapshot s = snapshot(rig.progress, day);
    CHECK(s == refs[completed] || s == refs[completed + 1]);
  }
}

// The card stops answering at every size() and read() call of an open, in
// every state an open can find (clean, a tail to replay, headers to rebuild,
// a short log to salvage, items.bin missing, nothing at all). size() then
// answers -1, as for a missing file, and available() turns false: nothing
// may be created, replaced or removed after that, and once the card is back
// a clean open finds the same state as if nothing had happened.
void testCardDropsOut() {
  const FakeCatalog catalog = FakeCatalog::vocab(30, 5);
  MemStore history;
  FakeClock clock(1200);
  makeHistory(history, catalog, clock, 3);

  std::vector<std::pair<const char*, MemStore>> cases;
  cases.push_back({"clean", history});
  {
    MemStore s = history;
    {
      Rig rig(s, catalog);
      rig.open();
      s.cutAt(s.calls + 1, MemStore::Tear::Nothing);  // journal lands, header does not
      rig.progress.review(2, Grade::Good, 0, 0, clock.today(), 0);
    }
    s.powerOn();
    cases.push_back({"tail", s});
  }
  {
    MemStore s = history;
    auto& f = s.files[ProgressStore::kItemsFile];
    for (int i = 0; i < 80; ++i) f[i] = f[512 + i] = 0x33;
    cases.push_back({"headers", s});
  }
  {
    MemStore s = history;
    s.files[ProgressStore::kJournalFile].resize(12 * 5);
    cases.push_back({"short log", s});
  }
  {
    MemStore s = history;
    s.files.erase(ProgressStore::kItemsFile);
    cases.push_back({"no items.bin", s});
  }
  cases.push_back({"empty", MemStore()});

  int drops = 0;
  for (auto& [name, base] : cases) {
    MemStore probe = base;
    Snapshot reference;
    int sizes = 0, reads = 0;
    {
      Rig rig(probe, catalog);
      const int s0 = probe.sizeCalls, r0 = probe.readCalls;
      CHECK(rig.open() != OpenResult::Failed);
      sizes = probe.sizeCalls - s0;
      reads = probe.readCalls - r0;
      reference = snapshot(rig.progress, clock.today());
    }
    for (int which = 0; which < 2; ++which) {
      const int count = which == 0 ? sizes : reads;
      for (int k = 0; k < count; ++k) {
        MemStore store = base;
        {
          Rig rig(store, catalog);
          if (which == 0) {
            store.dropAtSize(store.sizeCalls + k);
          } else {
            store.dropAtRead(store.readCalls + k);
          }
          rig.open();
          CHECK(store.gone());
          if (store.attemptsWhileGone != 0) {
            std::printf("  %s: %s call %d dropped, then %d write(s) tried\n", name, which == 0 ? "size" : "read", k,
                        store.attemptsWhileGone);
          }
          CHECK_EQ(store.attemptsWhileGone, 0);
          // A store that saw the card go refuses changes.
          if (rig.progress.failed()) {
            CHECK(rig.progress.review(0, Grade::Good, 0, 0, clock.today(), 0).status == Status::Failed);
          }
        }
        store.powerOn();
        Rig rig(store, catalog);
        CHECK(rig.open() != OpenResult::Failed);
        if (snapshot(rig.progress, clock.today()) != reference) {
          std::printf("  %s: %s call %d dropped: state differs after the card returned\n", name,
                      which == 0 ? "size" : "read", k);
        }
        CHECK(snapshot(rig.progress, clock.today()) == reference);
        ++drops;
      }
    }
  }
  std::printf("  %d card drop-outs during open\n", drops);
}

}  // namespace

int main() {
  testAuthoritativeRecoveryPrecedesLocalReads();
  testMutationJournalPrecedesLocalWrites();
  testCreateAndReopen();
  testUndo();
  testUndoReviewProof();
  testFlags();
  testForecastAndTotals();
  testRebuild();
  testRetiredAndReordered();
  testGuest();
  testJournalTail();
  testPowerCuts();
  testCardDropsOut();
  return tinta_test::result();
}

// modules: stats
//
// days.bin totals and the streak, including torn writes and date jumps.

#include "check.h"
#include "core/stats/DayLog.h"
#include "core/stats/Streak.h"
#include "fakes.h"

using namespace tinta::core;
using namespace tinta_test;

namespace {

DayTotals session(uint32_t reviews, uint32_t correct, uint32_t fresh, uint32_t seconds) {
  DayTotals t;
  t.reviews = reviews;
  t.correct = correct;
  t.newItems = fresh;
  t.seconds = seconds;
  return t;
}

void studyRange(DayLog& log, DayNumber from, DayNumber to) {
  for (uint32_t d = from; d <= to; ++d) log.add(static_cast<DayNumber>(d), session(5, 4, 1, 60));
}

StreakInfo streak(DayLog& log, DayNumber today) {
  StreakInfo s;
  CHECK(currentStreak(log, today, s));
  return s;
}

void testTotals() {
  MemStore store;
  DayLog log(store);
  DayTotals t;
  CHECK(log.totals(100, t));
  CHECK_EQ(t.reviews, 0);

  CHECK(log.add(100, session(30, 25, 10, 600)));
  CHECK(log.add(100, session(12, 12, 0, 200)));
  CHECK(log.add(101, session(8, 6, 2, 100)));
  CHECK_EQ(store.size(DayLog::kFile), 4 + 3 * 12);
  CHECK(log.totals(100, t));
  CHECK_EQ(t.reviews, 42);
  CHECK_EQ(t.correct, 37);
  CHECK_EQ(t.newItems, 10);
  CHECK_EQ(t.seconds, 800);

  DayTotals days[14];
  CHECK(log.range(95, 14, days));
  CHECK_EQ(days[5].reviews, 42);
  CHECK_EQ(days[6].reviews, 8);
  CHECK_EQ(days[0].reviews, 0);
  CHECK_EQ(days[13].reviews, 0);

  // Out of order (a date moved back) is fine.
  CHECK(log.add(97, session(3, 3, 0, 30)));
  uint32_t studied = 0;
  CHECK(log.allTime(t, &studied));
  CHECK_EQ(t.reviews, 53);
  CHECK_EQ(studied, 3);

  // Values beyond 16 bits split over records.
  CHECK(log.add(200, session(70000, 1, 2, 100000)));
  CHECK(log.totals(200, t));
  CHECK_EQ(t.reviews, 70000);
  CHECK_EQ(t.seconds, 100000);
  CHECK_EQ(t.newItems, 2);

  // Distinct days far apart.
  CHECK(log.add(5000, session(1, 1, 0, 1)));
  CHECK(log.allTime(t, &studied));
  CHECK_EQ(studied, 5);
}

void testTornAndForeign() {
  MemStore store;
  DayLog log(store);
  log.add(10, session(10, 9, 1, 50));
  // A torn append: half a record lands.
  store.cutAt(store.calls, MemStore::Tear::Prefix);
  CHECK(!log.add(11, session(20, 20, 2, 70)));
  store.powerOn();
  DayTotals t;
  CHECK(log.totals(11, t));
  CHECK_EQ(t.reviews, 0);
  // The next session overwrites the torn half; earlier days are intact.
  CHECK(log.add(11, session(4, 4, 0, 10)));
  CHECK_EQ(store.size(DayLog::kFile), 4 + 2 * 12);
  CHECK(log.totals(10, t));
  CHECK_EQ(t.reviews, 10);
  CHECK(log.totals(11, t));
  CHECK_EQ(t.reviews, 4);

  // A garbage record is skipped by its check.
  store.cutAt(store.calls, MemStore::Tear::Garbage);
  log.add(12, session(1, 1, 1, 1));
  store.powerOn();
  CHECK(log.totals(12, t));
  CHECK_EQ(t.reviews, 0);
  CHECK(log.totals(11, t));
  CHECK_EQ(t.reviews, 4);

  // Not a log this firmware wrote: replaced on the next add.
  store.files[DayLog::kFile] = {'j', 'u', 'n', 'k', 1, 2, 3};
  CHECK(log.totals(10, t));
  CHECK_EQ(t.reviews, 0);
  CHECK(log.add(13, session(2, 2, 0, 5)));
  CHECK_EQ(store.size(DayLog::kFile), 16);
  CHECK(log.totals(13, t));
  CHECK_EQ(t.reviews, 2);

  // The card pulled: StateStore reports the file as missing, so lookups see
  // an empty log and writes fail.
  store.failFrom(store.calls);
  CHECK(log.totals(13, t));
  CHECK_EQ(t.reviews, 0);
  CHECK(!log.add(13, session(1, 1, 0, 1)));
}

void testGuest() {
  MemStore store;
  store.present = false;
  DayLog log(store);
  log.add(50, session(10, 8, 3, 100));
  log.add(50, session(5, 5, 0, 50));
  DayTotals t;
  CHECK(log.totals(50, t));
  CHECK_EQ(t.reviews, 15);
  StreakInfo s = streak(log, 50);
  CHECK_EQ(s.days, 1);
  CHECK(s.studiedToday);
  log.add(51, session(1, 1, 0, 5));
  CHECK(log.totals(50, t));
  CHECK_EQ(t.reviews, 0);  // only the current day is kept
  CHECK(store.files.empty());
}

void testStreak() {
  {
    MemStore store;
    DayLog log(store);
    CHECK_EQ(streak(log, 0).days, 0);
    CHECK_EQ(streak(log, 300).days, 0);
    studyRange(log, 100, 110);
    StreakInfo s = streak(log, 110);
    CHECK_EQ(s.days, 11);
    CHECK(s.studiedToday);
    s = streak(log, 111);  // not yet today: alive from yesterday
    CHECK_EQ(s.days, 11);
    CHECK(!s.studiedToday);
    CHECK_EQ(streak(log, 112).days, 0);  // a missed day ends it
    CHECK_EQ(streak(log, 105).days, 6);  // as seen on day 105
  }
  {  // Longer than one window of days, and starting at day 0.
    MemStore store;
    DayLog log(store);
    studyRange(log, 0, 599);
    CHECK_EQ(streak(log, 599).days, 600);
    CHECK_EQ(streak(log, 600).days, 600);
    CHECK_EQ(streak(log, 255).days, 256);
    CHECK_EQ(streak(log, 256).days, 257);
    CHECK_EQ(streak(log, 0).days, 1);
  }
  {  // A session without grades (time only) is not a study day.
    MemStore store;
    DayLog log(store);
    log.add(20, session(0, 0, 0, 30));
    CHECK_EQ(streak(log, 20).days, 0);
  }
  {  // X4 date jumps.
    MemStore store;
    DayLog log(store);
    studyRange(log, 100, 105);
    // The learner confirmed a date far ahead by mistake and studied, then
    // corrected it: the stray record does not break or inflate the streak.
    log.add(900, session(5, 5, 0, 60));
    log.add(106, session(5, 5, 0, 60));
    CHECK_EQ(streak(log, 106).days, 7);
    // Days away, entered correctly: the streak restarts.
    log.add(110, session(5, 5, 0, 60));
    StreakInfo s = streak(log, 110);
    CHECK_EQ(s.days, 1);
    CHECK(s.studiedToday);
    // A date moved backwards by mistake: the streak is the one ending there.
    CHECK_EQ(streak(log, 103).days, 4);
  }
}

// The card stops answering at each size() or read() call of an add(): the
// log must not be replaced (which a -1 taken for "missing" would do).
void testCardDropsOut() {
  MemStore base;
  {
    DayLog log(base);
    studyRange(log, 300, 309);
  }
  const auto before = base.files;
  int drops = 0;
  for (int which = 0; which < 2; ++which) {
    for (int k = 0; k < 4; ++k) {
      MemStore store = base;
      DayLog log(store);
      if (which == 0) {
        store.dropAtSize(store.sizeCalls + k);
      } else {
        store.dropAtRead(store.readCalls + k);
      }
      const bool added = log.add(310, session(5, 5, 1, 60));
      if (!store.gone()) continue;  // add() finished before call k
      ++drops;
      CHECK(!added);
      CHECK_EQ(store.attemptsWhileGone, 0);
      CHECK(store.files == before);
      store.powerOn();
      CHECK_EQ(streak(log, 309).days, 10);
    }
  }
  CHECK(drops >= 2);

  // Gone before the add: the session goes to the in-RAM day instead.
  MemStore store = base;
  DayLog log(store);
  store.dropAtSize(store.sizeCalls);
  store.size(DayLog::kFile);
  CHECK(log.add(310, session(5, 5, 1, 60)));
  CHECK_EQ(store.attemptsWhileGone, 0);
  CHECK(store.files == before);
}

}  // namespace

int main() {
  testTotals();
  testTornAndForeign();
  testGuest();
  testStreak();
  testCardDropsOut();
  return tinta_test::result();
}

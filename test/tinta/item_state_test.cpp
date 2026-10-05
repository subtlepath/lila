// modules: srs
//
// ItemState packing and the session-relative step rules of Review.cpp.

#include <cmath>
#include <cstring>

#include "check.h"
#include "core/srs/Fsrs.h"
#include "core/srs/ItemState.h"
#include "core/srs/ProgressStore.h"
#include "core/srs/Review.h"

using namespace tinta::core;

namespace {

void testLayout() {
  ItemState s;
  s.uid = 0x11223344;
  s.dueDay = 0x5566;
  s.lastDay = 0x7788;
  s.stability = 0x99AA;
  s.difficulty = 0xBB;
  s.setPhase(Phase::Relearning, 1);
  s.reps = 0xCC;
  s.lapses = 0xDD;
  s.flags = item_flag::kSuspended | item_flag::kStarred;

  uint8_t bytes[16];
  s.encode(bytes);
  const uint8_t expected[16] = {0x44, 0x33, 0x22, 0x11, 0x66, 0x55, 0x88, 0x77,
                                0xAA, 0x99, 0xBB, 0x07, 0xCC, 0xDD, 0x05, 0x00};
  CHECK(std::memcmp(bytes, expected, 16) == 0);

  ItemState back;
  CHECK(ItemState::decode(bytes, back));
  CHECK(back == s);
  CHECK(back.phaseKind() == Phase::Relearning);
  CHECK_EQ(back.step(), 1);
  CHECK(back.suspended());

  ItemState fresh = ItemState::fresh(42);
  fresh.encode(bytes);
  CHECK(ItemState::decode(bytes, back));
  CHECK(back == fresh);
  CHECK(back.isNew());
}

void testRejects() {
  ItemState good;
  good.uid = 7;
  good.setStabilityDays(3.0f);
  good.setDifficultyValue(5.0f);
  good.setPhase(Phase::Review);
  good.reps = 2;
  uint8_t bytes[16];
  ItemState out;

  good.encode(bytes);
  CHECK(ItemState::decode(bytes, out));

  good.encode(bytes);
  bytes[11] |= 0x80;  // reserved phase bits
  CHECK(!ItemState::decode(bytes, out));

  good.encode(bytes);
  bytes[15] = 0x01;  // reserved flag bits
  CHECK(!ItemState::decode(bytes, out));

  good.encode(bytes);
  bytes[8] = bytes[9] = 0;  // graded with no stability
  CHECK(!ItemState::decode(bytes, out));

  good.encode(bytes);
  bytes[10] = 251;  // difficulty above 10
  CHECK(!ItemState::decode(bytes, out));

  ItemState fresh = ItemState::fresh(9);
  fresh.encode(bytes);
  bytes[8] = 1;  // new with stability
  CHECK(!ItemState::decode(bytes, out));

  std::memset(bytes, 0, sizeof bytes);  // a zeroed record is a fresh uid 0
  CHECK(ItemState::decode(bytes, out));
  CHECK(out.isNew());
}

void testFixedPoint() {
  ItemState s;
  s.setStabilityDays(2.3065f);
  CHECK_EQ(s.stability, 37);
  CHECK_NEAR(s.stabilityDays(), 2.3125, 1e-6);
  s.setStabilityDays(0.001f);
  CHECK_EQ(s.stability, 1);
  s.setStabilityDays(0.0f);
  CHECK_EQ(s.stability, 1);
  s.setStabilityDays(-5.0f);
  CHECK_EQ(s.stability, 1);
  s.setStabilityDays(std::nanf(""));
  CHECK_EQ(s.stability, 1);
  s.setStabilityDays(4095.9f);
  CHECK_EQ(s.stability, 65534);
  s.setStabilityDays(1e9f);
  CHECK_EQ(s.stability, 65535);
  s.setStabilityDays(INFINITY);
  CHECK_EQ(s.stability, 65535);

  for (float days = 0.0625f; days < 4000.0f; days *= 1.37f) {
    s.setStabilityDays(days);
    CHECK(std::fabs(s.stabilityDays() - days) <= 1.0f / 32.0f + 1e-4f);
  }

  s.setDifficultyValue(1.0f);
  CHECK_EQ(s.difficulty, 25);
  s.setDifficultyValue(10.0f);
  CHECK_EQ(s.difficulty, 250);
  s.setDifficultyValue(0.2f);
  CHECK_EQ(s.difficulty, 25);
  s.setDifficultyValue(42.0f);
  CHECK_EQ(s.difficulty, 250);
  s.setDifficultyValue(5.5f);
  CHECK_EQ(s.difficulty, 138);
  CHECK_NEAR(s.difficultyValue(), 5.52, 1e-6);

  s.setPhase(Phase::Learning, 200);
  CHECK_EQ(s.step(), ItemState::kMaxStep);
  CHECK(s.phaseKind() == Phase::Learning);
}

void testSteps() {
  const Fsrs fsrs;
  const DayNumber day = 900;

  // New, Good: step 0 (back after 3), Good: step 1 (after 8), Good: graduates.
  ItemState s = ItemState::fresh(1);
  ReviewOutcome o = applyReview(fsrs, s, Grade::Good, day);
  CHECK(o.inSession);
  CHECK_EQ(o.gap, 3);
  CHECK(s.phaseKind() == Phase::Learning);
  CHECK_EQ(s.step(), 0);
  CHECK_EQ(s.dueDay, day);
  CHECK_EQ(s.reps, 1);
  o = applyReview(fsrs, s, Grade::Good, day);
  CHECK(o.inSession);
  CHECK_EQ(o.gap, 8);
  CHECK_EQ(s.step(), 1);
  o = applyReview(fsrs, s, Grade::Hard, day);  // Hard repeats the step
  CHECK(o.inSession);
  CHECK_EQ(o.gap, 8);
  CHECK_EQ(s.step(), 1);
  o = applyReview(fsrs, s, Grade::Good, day);
  CHECK(!o.inSession);
  CHECK(s.phaseKind() == Phase::Review);
  CHECK(o.intervalDays >= 1);
  CHECK_EQ(s.dueDay, day + o.intervalDays);
  CHECK_EQ(s.lastDay, day);
  CHECK_EQ(s.reps, 4);
  CHECK_EQ(s.lapses, 0);

  // Again in the steps restarts them.
  ItemState t = ItemState::fresh(2);
  applyReview(fsrs, t, Grade::Good, day);
  applyReview(fsrs, t, Grade::Good, day);
  o = applyReview(fsrs, t, Grade::Again, day);
  CHECK_EQ(o.gap, 3);
  CHECK_EQ(t.step(), 0);
  CHECK_EQ(t.lapses, 0);  // not a lapse: it never graduated

  // Easy graduates a new item at once, with FSRS's Easy stability.
  ItemState e = ItemState::fresh(3);
  o = applyReview(fsrs, e, Grade::Easy, day);
  CHECK(!o.inSession);
  CHECK_EQ(o.intervalDays, fsrs.interval(fsrs.initial(Grade::Easy).stability));
  CHECK_NEAR(e.stabilityDays(), 8.2956, 1.0 / 32);

  // A lapse: Review + Again goes to relearning and counts.
  o = applyReview(fsrs, e, Grade::Again, static_cast<DayNumber>(day + o.intervalDays));
  CHECK(o.lapse);
  CHECK(o.inSession);
  CHECK(e.phaseKind() == Phase::Relearning);
  CHECK_EQ(e.lapses, 1);
  o = applyReview(fsrs, e, Grade::Good, e.lastDay);
  o = applyReview(fsrs, e, Grade::Good, e.lastDay);
  CHECK(e.phaseKind() == Phase::Review);

  // Leech at the sixth lapse, reported once.
  int leechReports = 0;
  for (int i = 0; i < 8; ++i) {
    const DayNumber d = e.dueDay;
    o = applyReview(fsrs, e, Grade::Again, d);
    if (o.becameLeech) ++leechReports;
    if (e.lapses == kLeechLapses - 1) CHECK((e.flags & item_flag::kLeech) == 0);
    applyReview(fsrs, e, Grade::Easy, d);
  }
  CHECK_EQ(leechReports, 1);
  CHECK((e.flags & item_flag::kLeech) != 0);
  CHECK_EQ(e.lapses, 9);

  // A date that moved backwards is a same-day review, not a negative gap.
  ItemState b = ItemState::fresh(4);
  applyReview(fsrs, b, Grade::Easy, day);
  ItemState same = b;
  applyReview(fsrs, b, Grade::Good, static_cast<DayNumber>(day - 3));
  applyReview(fsrs, same, Grade::Good, day);
  CHECK_EQ(b.stability, same.stability);
  CHECK_EQ(b.difficulty, same.difficulty);
}

void testJournalEntry() {
  JournalEntry e = JournalEntry::review(0xDEADBEEF, Grade::Hard, 9, 1600, 812, 70000000);
  uint8_t bytes[12];
  e.encode(bytes);
  const JournalEntry d = JournalEntry::decode(bytes);
  CHECK_EQ(d.uid, 0xDEADBEEFu);
  CHECK_EQ(d.time, 70000000u);
  CHECK_EQ(d.day, 812);
  CHECK(d.isReview());
  CHECK(d.grade() == Grade::Hard);
  CHECK_EQ(d.format(), 9);
  CHECK_EQ(d.arg, 6);  // 1.6 s in quarter seconds
  CHECK_EQ(d.controlCode(), 0);
  CHECK_EQ(JournalEntry::review(1, Grade::Good, 0, 0, 0, 0).arg, 0);
  CHECK_EQ(JournalEntry::review(1, Grade::Good, 0, 30, 0, 0).arg, 1);
  CHECK_EQ(JournalEntry::review(1, Grade::Good, 0, 600000, 0, 0).arg, 255);

  const JournalEntry u = JournalEntry::control(5, JournalEntry::kUndo, 0, 1, 2);
  CHECK(!u.isReview());
  CHECK_EQ(u.controlCode(), JournalEntry::kUndo);
  uint8_t zeros[12] = {};
  const JournalEntry z = JournalEntry::decode(zeros);
  CHECK(!z.isReview());
  CHECK_EQ(z.controlCode(), 0);
}

}  // namespace

int main() {
  testLayout();
  testRejects();
  testFixedPoint();
  testSteps();
  testJournalEntry();
  return tinta_test::result();
}

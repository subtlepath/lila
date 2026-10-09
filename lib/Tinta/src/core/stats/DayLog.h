#pragma once

#include <cstdint>

#include "core/Clock.h"
#include "core/StateStore.h"

namespace tinta::core {

struct DayTotals {
  uint32_t reviews = 0;   // grades given
  uint32_t correct = 0;   // grades other than Again
  uint32_t newItems = 0;  // items graded for the first time
  uint32_t seconds = 0;   // time spent answering

  DayTotals& operator+=(const DayTotals& o) {
    reviews += o.reviews;
    correct += o.correct;
    newItems += o.newItems;
    seconds += o.seconds;
    return *this;
  }
  bool studied() const { return reviews > 0; }
};

// Per-day study totals in days.bin (PLAN.md 8.3): a 4-byte magic "TDL1", then
// one 12-byte record per session, appended (one write per session):
//
//   0 day u16  2 reviews u16  4 correct u16  6 newItems u16  8 seconds u16
//  10 check u16 (low half of the CRC-32 of bytes 0..9)
//
// A day's totals are the sum of its records. Appending rather than updating
// in place means a torn write can only lose the session being written, and
// a learner-confirmed date that jumps backwards (X4) needs no special case:
// records are not assumed to be in day order. Lookups scan the file, about
// 4 KB a year at three sessions a day.
//
// Without a card the log keeps one day in RAM, so the home screen can still
// show today's progress.
class DayLog {
 public:
  static constexpr const char* kFile = "days.bin";
  static constexpr uint32_t kRecordSize = 12;
  static constexpr uint32_t kHeaderSize = 4;

  explicit DayLog(StateStore& store) : store_(store) {}

  // Adds a session's totals to `day`. Values beyond 16 bits are split over
  // several records.
  bool add(DayNumber day, const DayTotals& delta);
  // A false result may include committed records. Retain the uncertain delta
  // until authoritative recovery; appending it again can double-count totals.
  bool addChecked(DayNumber day, const DayTotals& delta);
  bool hasUncertainWrite() const { return uncertainWrite_; }
  DayNumber uncertainDay() const { return uncertainDay_; }
  const DayTotals& uncertainDelta() const { return uncertainDelta_; }

  bool totals(DayNumber day, DayTotals& out);
  // out[i] = totals of day first + i, for i < count.
  bool range(DayNumber first, uint16_t count, DayTotals* out);
  // Everything, and the number of distinct days studied.
  bool allTime(DayTotals& out, uint32_t* studyDays);
  // Sets bit i of `bits` (LSB first) when day first + i has study; `bits`
  // holds (count + 7) / 8 bytes and is cleared first.
  bool studyDays(DayNumber first, uint16_t count, uint8_t* bits);

 private:
  template <class Visit>
  bool forEach(Visit&& visit);
  bool writeRecord(DayNumber day, const DayTotals& part);

  StateStore& store_;
  DayNumber guestDay_ = 0;
  DayTotals guestTotals_;
  DayTotals uncertainDelta_;
  DayNumber uncertainDay_ = 0;
  bool uncertainWrite_ = false;
};

}  // namespace tinta::core

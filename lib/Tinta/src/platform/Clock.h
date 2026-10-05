#pragma once

// The learner's time (PLAN.md 6.8), behind core::Clock.
//
// Where lila's RTC reads a plausible time, local time is lila's: the RTC keeps
// UTC and lila's time zone (Settings > Clock, with daylight saving) gives the
// wall time; the study day rolls over at the profile's rollover hour (04:00 by
// default). Otherwise (the X4, which has no RTC, or an RTC that was never set,
// or reads earlier than the firmware date or the last day the device saw)
// the learner confirms the date, which holds for the rest of the power-on. No
// time of day then.

#include <stdint.h>

#include "core/Clock.h"
#include "core/Date.h"

namespace tinta::platform {

// The firmware's own date: an RTC reading before it means the clock was reset
// or never set. Update it when cutting a release.
inline constexpr core::DayNumber kFirmwareDay = core::date::dayNumber({2026, 10, 3});

struct LocalTime {
  core::date::Civil date{};
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
};

class Clock final : public core::Clock {
 public:
  void begin(bool boardHasRtc);

  // At open: the rollover hour, and the latest day the device has seen (or
  // studied), before which an RTC reading is not trusted.
  void configure(uint8_t rolloverHour, core::DayNumber lastKnownDay);
  void setRolloverHour(uint8_t hour) { rolloverHour_ = hour; }

  core::DayNumber today() const override;
  // True while the RTC is the clock (see above).
  bool hasTimeOfDay() const override;
  uint32_t nowSeconds() const override;

  // False until the date is known: the RTC is trusted, or the learner has
  // confirmed a date this power-on. The app asks for it before scheduling.
  bool trusted() const;

  // Local wall time; false without a time of day.
  bool localTime(LocalTime& out) const;

  // The date the learner confirmed, for this power-on.
  void confirmDay(core::DayNumber day);

  // True once each time the study day changes (boot, a confirmed date, a
  // rollover at night); logs "[tinta] day <n> <yyyy-mm-dd>".
  bool pollDayChange();

 private:
  // Local seconds since 2024-01-01 from the RTC, or false.
  bool localSeconds(uint32_t& out) const;

  bool hasRtc_ = false;
  uint8_t rolloverHour_ = 4;
  core::DayNumber lastKnownDay_ = 0;
  uint32_t bootMs_ = 0;

  core::DayNumber lastReportedDay_ = 0;
  bool reported_ = false;
};

}  // namespace tinta::platform

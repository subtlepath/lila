#pragma once

// The learner's time (PLAN.md 6.8), behind core::Clock.
//
// Three backends, chosen at begin():
//   RTC        X3, X4 Classic, X4 Pro: the hardware RTC holds UTC; the
//              profile's UTC offset gives local time and the study day rolls
//              over at the profile's rollover hour (04:00 by default).
//   Asked      X4 (no RTC): the date the learner confirmed at power-on, kept
//              in profile.bin. No time of day.
//   Simulated  RTC devices in simulator bundles, where the SDK's Rtc does not
//              work: UTC is NVS tinta/sim_clock ("YYYY-MM-DDTHH:MM[:SS]",
//              seeded by the harness before the bundle loads) plus the time
//              since this boot; tinta/sim_clock_run = "0" freezes it, so
//              screens that show the time can be compared with goldens. No
//              sim_clock reads as an RTC that was never set.
// hasTimeOfDay() follows the board profile in every build, so the X4's date
// prompt runs in the simulator too.

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

  // From the profile, at boot and whenever those settings change. On RTC
  // devices `lastKnownDay` is the latest day the device has seen; a clock
  // reading before it is not trusted.
  void configure(int16_t utcOffsetMinutes, uint8_t rolloverHour, core::DayNumber lastKnownDay);

  core::DayNumber today() const override;
  bool hasTimeOfDay() const override { return hasRtc_; }
  uint32_t nowSeconds() const override;

  // RTC devices: false when the clock gave no usable reading — never set, its
  // oscillator stopped, or earlier than the firmware date or the last day the
  // device saw. The app then asks for the date before scheduling anything.
  // The X4: false until the learner has confirmed a date this power-on.
  bool trusted() const;

  // Local wall time; false without a time of day or a trusted clock.
  bool localTime(LocalTime& out) const;
  // RTC devices: sets the clock from local time (the RTC keeps UTC).
  bool setLocalTime(const LocalTime& time);

  // The X4's date for this power-on.
  void confirmDay(core::DayNumber day);

  // True once each time the study day changes (boot, a confirmed date, a
  // rollover at night); logs "[tinta] day <n> <yyyy-mm-dd>".
  bool pollDayChange();

 private:
  // UTC seconds since 2024-01-01, or false.
  bool readUtc(uint32_t& out) const;
  bool localSeconds(uint32_t& out) const;
  void refresh() const;

  bool hasRtc_ = false;
  int16_t utcOffsetMinutes_ = 0;
  uint8_t rolloverHour_ = 4;
  core::DayNumber lastKnownDay_ = 0;
  core::DayNumber confirmedDay_ = 0;
  bool dayConfirmed_ = false;
  uint32_t bootMs_ = 0;

  // The last reading and when it was taken; extrapolated with millis() so the
  // RTC is not read on every frame.
  mutable bool readingValid_ = false;
  mutable uint32_t readingUtc_ = 0;
  mutable uint32_t readingMs_ = 0;
  mutable bool everRead_ = false;

  core::DayNumber lastReportedDay_ = 0;
  bool reported_ = false;

#if TINTA_SIM
  bool simFrozen_ = false;
#endif
};

}  // namespace tinta::platform

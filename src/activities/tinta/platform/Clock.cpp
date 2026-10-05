#if LILA_TINTA

#include "platform/Clock.h"

#include <Arduino.h>
#include <HalClock.h>
#include <stdio.h>
#include <string.h>

#include "platform/Log.h"

namespace tinta::platform {
namespace {

namespace date = core::date;

// The RTC drifts by seconds a day at most; reading it once a minute and
// extrapolating with millis() keeps the I2C bus quiet during rendering.
constexpr uint32_t kRtcPollMs = 60000;

uint32_t utcSecondsFrom(const date::Civil& d, uint8_t hour, uint8_t minute, uint8_t second) {
  return static_cast<uint32_t>(date::dayNumber(d)) * 86400u + hour * 3600u + minute * 60u + second;
}

void splitSeconds(uint32_t seconds, LocalTime& out) {
  out.date = date::civil(static_cast<core::DayNumber>(seconds / 86400u));
  const uint32_t rest = seconds % 86400u;
  out.hour = static_cast<uint8_t>(rest / 3600u);
  out.minute = static_cast<uint8_t>(rest / 60u % 60u);
  out.second = static_cast<uint8_t>(rest % 60u);
}

// The X4 has no RTC: the date the learner confirmed holds for the rest of
// this power-on, so reopening Tinta does not ask again.
bool gDayConfirmed = false;
core::DayNumber gConfirmedDay = 0;

}  // namespace

void Clock::begin(const bool boardHasRtc) {
  hasRtc_ = boardHasRtc;
  bootMs_ = millis();
  if (!hasRtc_) {
    dayConfirmed_ = gDayConfirmed;
    confirmedDay_ = gConfirmedDay;
    return;
  }
  refresh();
}

void Clock::configure(const int16_t utcOffsetMinutes, const uint8_t rolloverHour, const core::DayNumber lastKnownDay) {
  utcOffsetMinutes_ = utcOffsetMinutes;
  rolloverHour_ = rolloverHour;
  lastKnownDay_ = lastKnownDay;
}

void Clock::refresh() const {
  Rtc::DateTime dt;
  date::Civil c;
  readingMs_ = millis();
  everRead_ = true;
  if (!halClock.utcNow(dt)) {
    readingValid_ = false;
    return;
  }
  c.year = dt.year;
  c.month = dt.month;
  c.day = dt.day;
  readingValid_ = date::valid(c) && dt.hour < 24 && dt.minute < 60 && dt.second < 60;
  if (readingValid_) readingUtc_ = utcSecondsFrom(c, dt.hour, dt.minute, dt.second);
}

bool Clock::readUtc(uint32_t& out) const {
  if (!hasRtc_) return false;
  if (!everRead_ || millis() - readingMs_ >= kRtcPollMs) refresh();
  if (!readingValid_) return false;
  out = readingUtc_ + (millis() - readingMs_) / 1000u;
  return true;
}

bool Clock::localSeconds(uint32_t& out) const {
  uint32_t utc = 0;
  if (!readUtc(utc)) return false;
  const int64_t local = static_cast<int64_t>(utc) + static_cast<int64_t>(utcOffsetMinutes_) * 60;
  out = local < 0 ? 0 : static_cast<uint32_t>(local);
  return true;
}

bool Clock::trusted() const {
  if (!hasRtc_) return dayConfirmed_;
  uint32_t local = 0;
  if (!localSeconds(local)) return false;
  if (local / 86400u < kFirmwareDay) return false;
  return date::studyDay(local, rolloverHour_) >= lastKnownDay_;
}

core::DayNumber Clock::today() const {
  if (!hasRtc_) return confirmedDay_;
  uint32_t local = 0;
  // An untrusted clock is never used for scheduling; the app asks for the date
  // first. Until then the firmware date stands in.
  if (!localSeconds(local)) return kFirmwareDay;
  return date::studyDay(local, rolloverHour_);
}

uint32_t Clock::nowSeconds() const {
  uint32_t local = 0;
  if (hasRtc_ && localSeconds(local)) return local;
  return static_cast<uint32_t>(today()) * 86400u + (millis() - bootMs_) / 1000u;
}

bool Clock::localTime(LocalTime& out) const {
  uint32_t local = 0;
  if (!localSeconds(local)) return false;
  splitSeconds(local, out);
  return true;
}

bool Clock::setLocalTime(const LocalTime& time) {
  if (!hasRtc_ || !date::valid(time.date)) return false;
  const int64_t utc64 = static_cast<int64_t>(utcSecondsFrom(time.date, time.hour, time.minute, time.second)) -
                        static_cast<int64_t>(utcOffsetMinutes_) * 60;
  const uint32_t utc = utc64 < 0 ? 0 : static_cast<uint32_t>(utc64);
  LocalTime u;
  splitSeconds(utc, u);
  Rtc::DateTime dt;
  dt.year = u.date.year;
  dt.month = u.date.month;
  dt.day = u.date.day;
  dt.hour = u.hour;
  dt.minute = u.minute;
  dt.second = u.second;
  // DayNumber 0 was a Monday; the RTC counts from Sunday.
  dt.weekday = static_cast<uint8_t>((date::weekday(date::dayNumber(u.date)) + 1) % 7);
  if (!halClock.setUtc(dt)) {
    log("clock: RTC write failed");
    return false;
  }
  refresh();
  log("clock set %04u-%02u-%02uT%02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  return readingValid_;
}

void Clock::confirmDay(const core::DayNumber day) {
  confirmedDay_ = day;
  dayConfirmed_ = true;
  gConfirmedDay = day;
  gDayConfirmed = true;
}

bool Clock::pollDayChange() {
  if (!trusted()) return false;
  const core::DayNumber day = today();
  if (reported_ && day == lastReportedDay_) return false;
  reported_ = true;
  lastReportedDay_ = day;
  const date::Civil c = date::civil(day);
  log("day %u %04u-%02u-%02u", day, c.year, c.month, c.day);
  return true;
}

}  // namespace tinta::platform

#endif  // LILA_TINTA

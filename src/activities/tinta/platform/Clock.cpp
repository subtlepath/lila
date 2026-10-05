#if LILA_TINTA

#include "platform/Clock.h"

#include <Arduino.h>
#include <HalClock.h>
#include <time.h>

#include "platform/Log.h"

namespace tinta::platform {
namespace {

namespace date = core::date;

// The date the learner confirmed holds for the rest of this power-on, so
// reopening Tinta does not ask again, and the RTC is not consulted after it.
bool gDayConfirmed = false;
core::DayNumber gConfirmedDay = 0;

}  // namespace

void Clock::begin(const bool boardHasRtc) {
  hasRtc_ = boardHasRtc;
  bootMs_ = millis();
}

void Clock::configure(const uint8_t rolloverHour, const core::DayNumber lastKnownDay) {
  rolloverHour_ = rolloverHour;
  lastKnownDay_ = lastKnownDay;
}

bool Clock::localSeconds(uint32_t& out) const {
  struct tm local;
  if (!hasRtc_ || !halClock.localTime(local)) return false;
  date::Civil c;
  c.year = static_cast<uint16_t>(local.tm_year + 1900);
  c.month = static_cast<uint8_t>(local.tm_mon + 1);
  c.day = static_cast<uint8_t>(local.tm_mday);
  if (!date::valid(c)) return false;
  out = static_cast<uint32_t>(date::dayNumber(c)) * 86400u + local.tm_hour * 3600u + local.tm_min * 60u +
        static_cast<uint32_t>(local.tm_sec);
  return true;
}

bool Clock::hasTimeOfDay() const {
  if (gDayConfirmed) return false;
  uint32_t local = 0;
  if (!localSeconds(local) || local / 86400u < kFirmwareDay) return false;
  return date::studyDay(local, rolloverHour_) >= lastKnownDay_;
}

bool Clock::trusted() const { return gDayConfirmed || hasTimeOfDay(); }

core::DayNumber Clock::today() const {
  uint32_t local = 0;
  if (hasTimeOfDay() && localSeconds(local)) return date::studyDay(local, rolloverHour_);
  if (gDayConfirmed) return gConfirmedDay;
  // Not used for scheduling: the app asks for the date first.
  return kFirmwareDay;
}

uint32_t Clock::nowSeconds() const {
  uint32_t local = 0;
  if (hasTimeOfDay() && localSeconds(local)) return local;
  return static_cast<uint32_t>(today()) * 86400u + (millis() - bootMs_) / 1000u;
}

bool Clock::localTime(LocalTime& out) const {
  uint32_t local = 0;
  if (!hasTimeOfDay() || !localSeconds(local)) return false;
  out.date = date::civil(static_cast<core::DayNumber>(local / 86400u));
  const uint32_t rest = local % 86400u;
  out.hour = static_cast<uint8_t>(rest / 3600u);
  out.minute = static_cast<uint8_t>(rest / 60u % 60u);
  out.second = static_cast<uint8_t>(rest % 60u);
  return true;
}

void Clock::confirmDay(const core::DayNumber day) {
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

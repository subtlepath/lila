#pragma once

#include <cstdint>

namespace tinta::core {

// The scheduler works in whole days, counted from 2024-01-01 (day 0).
using DayNumber = uint16_t;

// Time as the learning engine sees it. Devices with an RTC answer from it; the
// X4 has no clock and answers from the date the learner confirmed at power-on
// (PLAN.md section 6.8). Host tests supply a fake.
class Clock {
 public:
  virtual ~Clock() = default;

  // The learner's current study day.
  virtual DayNumber today() const = 0;

  // False when only the date is known (X4).
  virtual bool hasTimeOfDay() const = 0;

  // Seconds since 2024-01-01 00:00 local, for journal timestamps and response
  // times. Without a time of day this is today() * 86400 plus uptime.
  virtual uint32_t nowSeconds() const = 0;
};

}  // namespace tinta::core

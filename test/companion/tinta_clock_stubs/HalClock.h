#pragma once
#include <cstdint>
#include <ctime>
class HalClock {
 public:
  std::tm local{};
  uint64_t utc = 0;
  bool localAvailable = false;
  bool utcAvailable = false;
  bool localTime(std::tm& out) const {
    if (!localAvailable) return false;
    out = local;
    return true;
  }
  bool unixTime(uint64_t& out) const {
    if (!utcAvailable) return false;
    out = utc;
    return true;
  }
};
extern HalClock halClock;

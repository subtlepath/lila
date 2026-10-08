#pragma once

#include "CompanionReaderPreferencePlan.h"

namespace companion {
enum class ReaderPreferenceStoreResult : uint8_t { Ok, Conflict, IoError };
class ReaderPreferenceStore {
 public:
  virtual ~ReaderPreferenceStore() = default;
  virtual bool read(ReaderPreferenceValues& output) = 0;
  // Success means durable recoverable publication. Failure retains the previous
  // portable values; replacement preserves every device-specific setting.
  virtual ReaderPreferenceStoreResult replace(const ReaderPreferenceValues& expected,
                                              const ReaderPreferenceValues& replacement) = 0;
};
enum class ReaderPreferenceApplicationResult : uint8_t {
  Applied,
  Unchanged,
  InvalidBody,
  UnavailableDependency,
  Conflict,
  IoError
};
// Caller retains this owner outside the task stack and excludes local writers.
class ReaderPreferenceApplication final {
 public:
  ReaderPreferenceApplicationResult run(std::span<const std::span<const uint8_t>> bodies,
                                        ReaderPreferenceDependencies& dependencies, ReaderPreferenceStore& store) {
    if (!store.read(current)) return ReaderPreferenceApplicationResult::IoError;
    const auto planned = plan.run(bodies, current, dependencies);
    if (planned == ReaderPreferencePlanResult::InvalidBody) return ReaderPreferenceApplicationResult::InvalidBody;
    if (planned == ReaderPreferencePlanResult::UnavailableDependency)
      return ReaderPreferenceApplicationResult::UnavailableDependency;
    const auto* replacement = plan.values();
    if (!replacement) return ReaderPreferenceApplicationResult::InvalidBody;
    if (*replacement == current) return ReaderPreferenceApplicationResult::Unchanged;
    switch (store.replace(current, *replacement)) {
      case ReaderPreferenceStoreResult::Ok:
        return ReaderPreferenceApplicationResult::Applied;
      case ReaderPreferenceStoreResult::Conflict:
        return ReaderPreferenceApplicationResult::Conflict;
      case ReaderPreferenceStoreResult::IoError:
        return ReaderPreferenceApplicationResult::IoError;
    }
    return ReaderPreferenceApplicationResult::IoError;
  }

 private:
  ReaderPreferenceValues current;
  ReaderPreferencePlan plan;
};
}  // namespace companion

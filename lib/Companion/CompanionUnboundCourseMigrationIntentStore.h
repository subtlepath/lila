#pragma once

#include "CompanionUnboundCourseMigrationIntent.h"
#include "CompanionUnboundCourseMigrationPaths.h"

namespace companion {
enum class UnboundCourseIntentResult {
  Ok,
  Missing,
  Invalid,
  Busy,
  Pending,
  Conflict,
  Corrupt,
  IoError,
  VerificationFailed
};
// Retain off stack: record copies exceed the local frame budget. No internal allocation.
// Caller prepares parents, excludes writers and verifies each native phase before persistence.
// Immutable phase records retain the original authorization throughout recovery.
class UnboundCourseMigrationIntentStore final {
 public:
  using Permission = bool (*)(void*);
  using VerifyPhase = bool (*)(void*, const UnboundCourseMigrationIntent&);
  UnboundCourseMigrationIntentStore(TransferStorage& storage, std::span<uint8_t> scratch, Permission permitted,
                                    void* context)
      : storage(storage), scratch(scratch), permitted(permitted), context(context) {}
  UnboundCourseMigrationIntentStore(const UnboundCourseMigrationIntentStore&) = delete;
  UnboundCourseMigrationIntentStore& operator=(const UnboundCourseMigrationIntentStore&) = delete;
  UnboundCourseIntentResult load(UnboundCourseMigrationIntent& output) {
    if (operating) return UnboundCourseIntentResult::Busy;
    if (!arguments(&output, sizeof(output))) return UnboundCourseIntentResult::Invalid;
    operating = true;
    unsigned latest = 0;
    for (unsigned phase = 1; phase <= 3; ++phase) {
      auto result = read(PATHS[phase - 1]);
      if (result == UnboundCourseIntentResult::Ok) {
        if (phase != latest + 1 || static_cast<unsigned>(observed.phase) != phase ||
            (latest && !sameFamily(observed, expected)))
          return finish(UnboundCourseIntentResult::Conflict);
        expected = observed;
        latest = phase;
      } else if (result != UnboundCourseIntentResult::Missing)
        return finish(result);
      result = read(STAGES[phase - 1]);
      if (result == UnboundCourseIntentResult::Ok) return finish(UnboundCourseIntentResult::Pending);
      if (result != UnboundCourseIntentResult::Missing) return finish(result);
    }
    const auto result = finish(latest ? UnboundCourseIntentResult::Ok : UnboundCourseIntentResult::Missing);
    if (result == UnboundCourseIntentResult::Ok) output = expected;
    return result;
  }
  UnboundCourseIntentResult persist(const UnboundCourseMigrationIntent& input, VerifyPhase verifyTorn = nullptr,
                                    void* verificationContext = nullptr) {
    if (operating) return UnboundCourseIntentResult::Busy;
    if (!validUnboundCourseMigrationIntent(input) || !arguments(&input, sizeof(input)))
      return UnboundCourseIntentResult::Invalid;
    expected = input;
    operating = true;
    const auto target = static_cast<unsigned>(expected.phase);
    unsigned latest = 0;
    bool staged = false, torn = false;
    auto inspected = inspect(latest, staged, torn);
    if (inspected != UnboundCourseIntentResult::Ok) return finish(inspected);
    if (target <= latest) return finish(UnboundCourseIntentResult::Ok);
    if (target != latest + 1) return finish(UnboundCourseIntentResult::Conflict);
    if (torn) {
      if (!verifyTorn) return finish(UnboundCourseIntentResult::Corrupt);
      uint64_t length = 0;
      auto result = prefix(length);
      if (result != UnboundCourseIntentResult::Ok) return finish(result);
      if (!guard()) return finish(UnboundCourseIntentResult::Busy);
      if (!verifyTorn(verificationContext, expected)) return finish(UnboundCourseIntentResult::VerificationFailed);
      if (!guard()) return finish(UnboundCourseIntentResult::Busy);
      inspected = inspect(latest, staged, torn);
      if (inspected != UnboundCourseIntentResult::Ok) return finish(inspected);
      if (target != latest + 1 || staged || !torn) return finish(UnboundCourseIntentResult::Conflict);
      uint64_t rechecked = 0;
      result = prefix(rechecked);
      if (result != UnboundCourseIntentResult::Ok) return finish(result);
      if (length != rechecked) return finish(UnboundCourseIntentResult::Conflict);
      if (!guard()) return finish(UnboundCourseIntentResult::Busy);
      if (!storage.remove(STAGES[target - 1])) return finish(UnboundCourseIntentResult::IoError);
      if (!guard()) return finish(UnboundCourseIntentResult::Busy);
      uint64_t size = 0;
      if (storage.stat(STAGES[target - 1], size) != FileStatus::Missing)
        return finish(UnboundCourseIntentResult::IoError);
      if (!guard()) return finish(UnboundCourseIntentResult::Busy);
    }
    if (!staged) {
      auto bytes = scratch.first(UNBOUND_COURSE_MIGRATION_INTENT_SIZE);
      if (!encodeUnboundCourseMigrationIntent(expected, bytes)) return finish(UnboundCourseIntentResult::Invalid);
      if (!guard()) return finish(UnboundCourseIntentResult::Busy);
      if (!storage.write(STAGES[target - 1], 0, bytes, true)) return finish(UnboundCourseIntentResult::IoError);
    }
    auto result = read(STAGES[target - 1]);
    if (result != UnboundCourseIntentResult::Ok) return finish(result);
    if (observed != expected) return finish(UnboundCourseIntentResult::Conflict);
    uint64_t size = 0;
    if (!guard()) return finish(UnboundCourseIntentResult::Busy);
    const auto status = storage.stat(PATHS[target - 1], size);
    if (!guard()) return finish(UnboundCourseIntentResult::Busy);
    if (status == FileStatus::Error) return finish(UnboundCourseIntentResult::IoError);
    if (status != FileStatus::Missing) return finish(UnboundCourseIntentResult::Conflict);
    if (!storage.rename(STAGES[target - 1], PATHS[target - 1])) return finish(UnboundCourseIntentResult::IoError);
    result = read(PATHS[target - 1]);
    return finish(result == UnboundCourseIntentResult::Ok && observed != expected ? UnboundCourseIntentResult::Conflict
                                                                                  : result);
  }
  inline static constexpr const auto& PATHS = UNBOUND_COURSE_INTENT_PATHS;
  inline static constexpr const auto& STAGES = UNBOUND_COURSE_INTENT_STAGES;

 private:
  TransferStorage& storage;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  UnboundCourseMigrationIntent expected, observed;
  bool operating = false;
  bool guard() const { return permitted && permitted(context); }
  bool arguments(const void* value, size_t length) const {
    return scratch.size() >= UNBOUND_COURSE_MIGRATION_INTENT_SIZE &&
           !course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) &&
           !course_baseline_detail::overlaps(scratch.data(), scratch.size(), value, length) &&
           !course_baseline_detail::overlaps(this, sizeof(*this), value, length);
  }
  static bool sameFamily(const UnboundCourseMigrationIntent& a, const UnboundCourseMigrationIntent& b) {
    return a.reader == b.reader && a.request == b.request && a.activePack == b.activePack;
  }
  UnboundCourseIntentResult inspect(unsigned& latest, bool& staged, bool& torn) {
    latest = 0;
    staged = false;
    torn = false;
    const auto target = static_cast<unsigned>(expected.phase);
    for (unsigned phase = 1; phase <= 3; ++phase) {
      auto result = read(PATHS[phase - 1]);
      if (result == UnboundCourseIntentResult::Ok) {
        if (phase != latest + 1 || static_cast<unsigned>(observed.phase) != phase || !sameFamily(observed, expected))
          return UnboundCourseIntentResult::Conflict;
        latest = phase;
      } else if (result != UnboundCourseIntentResult::Missing)
        return result;
      result = read(STAGES[phase - 1]);
      if (result == UnboundCourseIntentResult::Ok) {
        if (phase != target || phase <= latest || observed != expected) return UnboundCourseIntentResult::Conflict;
        staged = true;
      } else if (result == UnboundCourseIntentResult::Corrupt && phase == target && phase > latest) {
        torn = true;
      } else if (result != UnboundCourseIntentResult::Missing)
        return result;
    }
    return UnboundCourseIntentResult::Ok;
  }
  UnboundCourseIntentResult prefix(uint64_t& length) {
    if (scratch.size() <= UNBOUND_COURSE_MIGRATION_INTENT_SIZE) return UnboundCourseIntentResult::Invalid;
    const auto* path = STAGES[static_cast<unsigned>(expected.phase) - 1];
    if (!guard()) return UnboundCourseIntentResult::Busy;
    if (storage.stat(path, length) != FileStatus::Present) return UnboundCourseIntentResult::IoError;
    if (!guard()) return UnboundCourseIntentResult::Busy;
    if (length >= UNBOUND_COURSE_MIGRATION_INTENT_SIZE) return UnboundCourseIntentResult::Corrupt;
    auto bytes = scratch.first(UNBOUND_COURSE_MIGRATION_INTENT_SIZE);
    if (!encodeUnboundCourseMigrationIntent(expected, bytes)) return UnboundCourseIntentResult::Invalid;
    auto buffer = scratch.subspan(UNBOUND_COURSE_MIGRATION_INTENT_SIZE);
    for (size_t offset = 0; offset < length;) {
      const auto count = std::min<uint64_t>(buffer.size(), length - offset);
      if (!guard()) return UnboundCourseIntentResult::Busy;
      if (!storage.read(path, offset, buffer.first(count))) return UnboundCourseIntentResult::IoError;
      if (!guard()) return UnboundCourseIntentResult::Busy;
      if (!std::equal(buffer.begin(), buffer.begin() + count, bytes.begin() + offset))
        return UnboundCourseIntentResult::Corrupt;
      offset += count;
    }
    return UnboundCourseIntentResult::Ok;
  }
  UnboundCourseIntentResult read(const char* path) {
    if (!guard()) return UnboundCourseIntentResult::Busy;
    uint64_t size = 0;
    const auto status = storage.stat(path, size);
    if (!guard()) return UnboundCourseIntentResult::Busy;
    if (status == FileStatus::Missing) return UnboundCourseIntentResult::Missing;
    if (status == FileStatus::Error) return UnboundCourseIntentResult::IoError;
    if (size != UNBOUND_COURSE_MIGRATION_INTENT_SIZE) return UnboundCourseIntentResult::Corrupt;
    auto bytes = scratch.first(UNBOUND_COURSE_MIGRATION_INTENT_SIZE);
    if (!storage.read(path, 0, bytes)) return UnboundCourseIntentResult::IoError;
    if (!guard()) return UnboundCourseIntentResult::Busy;
    return decodeUnboundCourseMigrationIntent(bytes, observed) ? UnboundCourseIntentResult::Ok
                                                               : UnboundCourseIntentResult::Corrupt;
  }
  UnboundCourseIntentResult finish(UnboundCourseIntentResult result) {
    operating = false;
    return guard() ? result : UnboundCourseIntentResult::Busy;
  }
};
}  // namespace companion

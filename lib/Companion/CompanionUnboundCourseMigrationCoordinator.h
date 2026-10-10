#pragma once

#include "CompanionUnboundCourseMigrationIntentStore.h"

namespace companion {
struct UnboundCourseMigrationHooks {
  void* context = nullptr;
  // Prepared: original/current pack compatibility, review and backups; recovery
  // may recognize only this intent's partial binding/isolation. Bound: exact
  // native binding and owned migration. Isolated: completed proofs and backups,
  // allowing newer learner state. Each check is read-only and authenticates identities.
  bool (*verify)(void*, const UnboundCourseMigrationIntent&, bool recovering) = nullptr;
  bool (*bind)(void*, const UnboundCourseMigrationIntent&) = nullptr;
  bool (*isolate)(void*, const UnboundCourseMigrationIntent&) = nullptr;
};
// Retain off stack; all record copies and paths share the caller's workspace.
// Caller supplies checked storage, prepares parents and excludes all state writers.
class UnboundCourseMigrationCoordinator final {
 public:
  using Permission = UnboundCourseMigrationIntentStore::Permission;
  UnboundCourseMigrationCoordinator(TransferStorage& storage, std::span<uint8_t> scratch, Permission permitted,
                                    void* context)
      : storage(storage),
        scratch(scratch),
        permitted(permitted),
        context(context),
        intents(storage, scratch, permitted, context) {}
  UnboundCourseMigrationCoordinator(const UnboundCourseMigrationCoordinator&) = delete;
  UnboundCourseMigrationCoordinator& operator=(const UnboundCourseMigrationCoordinator&) = delete;
  UnboundCourseIntentResult run(const UnboundCourseMigrationIntent& input, const UnboundCourseMigrationHooks& hooks) {
    if (operating) return UnboundCourseIntentResult::Busy;
    ready = false;
    if (!validUnboundCourseMigrationIntent(input) || input.phase != UnboundCourseMigrationPhase::Prepared ||
        scratch.size() <= UNBOUND_COURSE_MIGRATION_INTENT_SIZE || !hooks.verify || !hooks.bind || !hooks.isolate ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &input, sizeof(input)))
      return UnboundCourseIntentResult::Invalid;
    record = input;
    active = hooks;
    operating = true;
    unsigned latest = 0;
    bool pending = false;
    auto result = inspect(latest, pending);
    if (result != UnboundCourseIntentResult::Ok) return finish(result);
    if (!latest) {
      record.phase = UnboundCourseMigrationPhase::Prepared;
      if (!verify(pending)) return finish(UnboundCourseIntentResult::VerificationFailed);
      result = save();
      if (result != UnboundCourseIntentResult::Ok) return finish(result);
      latest = 1;
    }
    if (latest == 1) {
      record.phase = UnboundCourseMigrationPhase::Prepared;
      if (!verify(true)) return finish(UnboundCourseIntentResult::VerificationFailed);
      result = recheck(1);
      if (result != UnboundCourseIntentResult::Ok) return finish(result);
      if (!guard() || !active.bind(active.context, record)) return finish(UnboundCourseIntentResult::IoError);
      record.phase = UnboundCourseMigrationPhase::Bound;
      if (!verify(true)) return finish(UnboundCourseIntentResult::VerificationFailed);
      result = save();
      if (result != UnboundCourseIntentResult::Ok) return finish(result);
      latest = 2;
    }
    if (latest == 2) {
      record.phase = UnboundCourseMigrationPhase::Bound;
      if (!verify(true)) return finish(UnboundCourseIntentResult::VerificationFailed);
      result = recheck(2);
      if (result != UnboundCourseIntentResult::Ok) return finish(result);
      if (!guard() || !active.isolate(active.context, record)) return finish(UnboundCourseIntentResult::IoError);
      record.phase = UnboundCourseMigrationPhase::Isolated;
      if (!verify(true)) return finish(UnboundCourseIntentResult::VerificationFailed);
      result = save();
      if (result != UnboundCourseIntentResult::Ok) return finish(result);
    }
    record.phase = UnboundCourseMigrationPhase::Isolated;
    if (!verify(true)) return finish(UnboundCourseIntentResult::VerificationFailed);
    result = recheck(3);
    if (result != UnboundCourseIntentResult::Ok) return finish(result);
    return finish(UnboundCourseIntentResult::Ok);
  }
  const UnboundCourseMigrationIntent* completed() const {
    if (!guard()) ready = false;
    return ready ? &record : nullptr;
  }
  void close() { ready = false; }

 private:
  TransferStorage& storage;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  UnboundCourseMigrationIntentStore intents;
  UnboundCourseMigrationIntent record, observed;
  UnboundCourseMigrationHooks active;
  bool operating = false;
  mutable bool ready = false;
  bool guard() const { return permitted && permitted(context); }
  bool verify(bool recovering) { return guard() && active.verify(active.context, record, recovering) && guard(); }
  UnboundCourseIntentResult save() {
    return intents.persist(
        record,
        [](void* raw, const UnboundCourseMigrationIntent&) {
          return static_cast<UnboundCourseMigrationCoordinator*>(raw)->verify(true);
        },
        this);
  }
  bool sameFamily() const {
    return record.reader == observed.reader && record.request == observed.request &&
           record.activePack == observed.activePack;
  }
  UnboundCourseIntentResult recheck(unsigned required) {
    const auto phase = record.phase;
    unsigned latest = 0;
    bool pending = false;
    const auto result = inspect(latest, pending);
    record.phase = phase;
    if (result != UnboundCourseIntentResult::Ok) return result;
    return latest == required ? UnboundCourseIntentResult::Ok : UnboundCourseIntentResult::Conflict;
  }
  UnboundCourseIntentResult read(const char* path) {
    if (!guard()) return UnboundCourseIntentResult::Busy;
    auto bytes = scratch.first(UNBOUND_COURSE_MIGRATION_INTENT_SIZE);
    if (!storage.read(path, 0, bytes)) return UnboundCourseIntentResult::IoError;
    if (!guard()) return UnboundCourseIntentResult::Busy;
    return decodeUnboundCourseMigrationIntent(bytes, observed) ? UnboundCourseIntentResult::Ok
                                                               : UnboundCourseIntentResult::Corrupt;
  }
  UnboundCourseIntentResult inspect(unsigned& latest, bool& pending) {
    latest = 0;
    pending = false;
    for (unsigned phase = 1; phase <= 3; ++phase) {
      uint64_t size = 0;
      if (!guard()) return UnboundCourseIntentResult::Busy;
      const auto status = storage.stat(UNBOUND_COURSE_INTENT_PATHS[phase - 1], size);
      if (!guard()) return UnboundCourseIntentResult::Busy;
      if (status == FileStatus::Missing) continue;
      if (status == FileStatus::Error) return UnboundCourseIntentResult::IoError;
      if (size != UNBOUND_COURSE_MIGRATION_INTENT_SIZE) return UnboundCourseIntentResult::Corrupt;
      const auto result = read(UNBOUND_COURSE_INTENT_PATHS[phase - 1]);
      if (result != UnboundCourseIntentResult::Ok) return result;
      if (phase != latest + 1 || static_cast<unsigned>(observed.phase) != phase || !sameFamily())
        return UnboundCourseIntentResult::Conflict;
      latest = phase;
    }
    for (unsigned phase = 1; phase <= 3; ++phase) {
      uint64_t size = 0;
      if (!guard()) return UnboundCourseIntentResult::Busy;
      const auto status = storage.stat(UNBOUND_COURSE_INTENT_STAGES[phase - 1], size);
      if (!guard()) return UnboundCourseIntentResult::Busy;
      if (status == FileStatus::Missing) continue;
      if (status == FileStatus::Error) return UnboundCourseIntentResult::IoError;
      if (pending || phase != latest + 1 || size > UNBOUND_COURSE_MIGRATION_INTENT_SIZE)
        return UnboundCourseIntentResult::Conflict;
      pending = true;
      if (size == UNBOUND_COURSE_MIGRATION_INTENT_SIZE) {
        const auto result = read(UNBOUND_COURSE_INTENT_STAGES[phase - 1]);
        if (result != UnboundCourseIntentResult::Ok) return result;
        if (static_cast<unsigned>(observed.phase) != phase || !sameFamily()) return UnboundCourseIntentResult::Conflict;
      } else {
        record.phase = static_cast<UnboundCourseMigrationPhase>(phase);
        auto bytes = scratch.first(UNBOUND_COURSE_MIGRATION_INTENT_SIZE);
        if (!encodeUnboundCourseMigrationIntent(record, bytes)) return UnboundCourseIntentResult::Invalid;
        auto buffer = scratch.subspan(UNBOUND_COURSE_MIGRATION_INTENT_SIZE);
        for (size_t offset = 0; offset < size;) {
          const auto count = std::min<uint64_t>(buffer.size(), size - offset);
          if (!guard()) return UnboundCourseIntentResult::Busy;
          if (!storage.read(UNBOUND_COURSE_INTENT_STAGES[phase - 1], offset, buffer.first(count)))
            return UnboundCourseIntentResult::IoError;
          if (!guard()) return UnboundCourseIntentResult::Busy;
          if (!std::equal(buffer.begin(), buffer.begin() + count, bytes.begin() + offset))
            return UnboundCourseIntentResult::Corrupt;
          offset += count;
        }
      }
    }
    return UnboundCourseIntentResult::Ok;
  }
  UnboundCourseIntentResult finish(UnboundCourseIntentResult result) {
    if (!guard()) result = UnboundCourseIntentResult::Busy;
    ready = result == UnboundCourseIntentResult::Ok;
    operating = false;
    return result;
  }
};
}  // namespace companion

#pragma once

#include "CompanionCourseBaselinePublication.h"
#include "CompanionTransfer.h"

namespace companion {
enum class CourseBaselinePublicationResult {
  Ok,
  Missing,
  Invalid,
  Busy,
  Conflict,
  Corrupt,
  IoError,
  VerificationFailed,
  TornStage
};
struct CourseBaselinePublicationHooks {
  void* context = nullptr;
  // Initial verification requires no baseline and the live reviewed cohort.
  // Recovery may recognize only this intent's exact already-published archive.
  bool (*verifyPrepared)(void*, const CourseBaselinePublicationRecord&, bool recovering) = nullptr;
  bool (*publishArchive)(void*, const CourseBaselinePublicationRecord&) = nullptr;
  // Read-only verification of native consent, immutable review/copies and archive;
  // it must preserve newer learner state and does not grant fresh approval.
  bool (*verifyPublished)(void*, const CourseBaselinePublicationRecord&) = nullptr;
};
// Admit off stack. Caller lends scratch, provides checked synced storage with
// complete namespace lookups, and excludes all other state/namespace writers.
// Hooks borrow the copied Prepared record only for their call and may reuse scratch.
class CourseBaselinePublicationStore final {
 public:
  using Permission = bool (*)(void*);
  CourseBaselinePublicationStore(TransferStorage& storage, std::span<uint8_t> scratch, Permission permitted,
                                 void* context)
      : storage(storage), scratch(scratch), permitted(permitted), context(context) {}
  CourseBaselinePublicationStore(const CourseBaselinePublicationStore&) = delete;
  CourseBaselinePublicationStore& operator=(const CourseBaselinePublicationStore&) = delete;
  CourseBaselinePublicationResult publish(const CourseBaselinePublicationRecord& request,
                                          const CourseBaselinePublicationHooks& hooks) {
    if (operating) return CourseBaselinePublicationResult::Busy;
    ready = false;
    if (request.phase != CourseBaselinePublicationPhase::Prepared || !validCourseBaselinePublicationRecord(request) ||
        scratch.size() < COURSE_BASELINE_PUBLICATION_SIZE ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        !hooks.verifyPrepared || !hooks.publishArchive || !hooks.verifyPublished)
      return CourseBaselinePublicationResult::Invalid;
    expected = request;
    active = hooks;
    operating = true;
    paths();
    if (!guard()) return finish(CourseBaselinePublicationResult::Busy);
    const auto prepared = read(CourseBaselinePublicationPhase::Prepared, false);
    if (!available(prepared)) return finish(prepared);
    const auto preparedStage = read(CourseBaselinePublicationPhase::Prepared, true);
    if (!available(preparedStage)) return finish(preparedStage);
    const auto published = read(CourseBaselinePublicationPhase::Published, false);
    if (!available(published)) return finish(published);
    const auto publishedStage = read(CourseBaselinePublicationPhase::Published, true);
    if (!available(publishedStage)) return finish(publishedStage);
    if ((preparedStage == CourseBaselinePublicationResult::TornStage &&
         (prepared != CourseBaselinePublicationResult::Missing ||
          published != CourseBaselinePublicationResult::Missing ||
          publishedStage != CourseBaselinePublicationResult::Missing)) ||
        (publishedStage == CourseBaselinePublicationResult::TornStage &&
         (prepared != CourseBaselinePublicationResult::Ok ||
          preparedStage != CourseBaselinePublicationResult::Missing)))
      return finish(CourseBaselinePublicationResult::Conflict);
    if ((prepared == CourseBaselinePublicationResult::Ok &&
         preparedStage != CourseBaselinePublicationResult::Missing) ||
        (published == CourseBaselinePublicationResult::Ok &&
         publishedStage != CourseBaselinePublicationResult::Missing))
      return finish(CourseBaselinePublicationResult::Conflict);
    if (published == CourseBaselinePublicationResult::Ok || publishedStage == CourseBaselinePublicationResult::Ok) {
      if (prepared != CourseBaselinePublicationResult::Ok || preparedStage != CourseBaselinePublicationResult::Missing)
        return finish(CourseBaselinePublicationResult::Conflict);
      if (!guard() || !active.verifyPublished(active.context, expected))
        return finish(CourseBaselinePublicationResult::VerificationFailed);
      if (published == CourseBaselinePublicationResult::Missing) {
        const auto saved = save(CourseBaselinePublicationPhase::Published);
        if (saved != CourseBaselinePublicationResult::Ok) return finish(saved);
        if (!guard() || !active.verifyPublished(active.context, expected))
          return finish(CourseBaselinePublicationResult::VerificationFailed);
      }
      return finish(CourseBaselinePublicationResult::Ok);
    }
    if (!guard() || !active.verifyPrepared(active.context, expected, prepared == CourseBaselinePublicationResult::Ok))
      return finish(CourseBaselinePublicationResult::VerificationFailed);
    if (preparedStage == CourseBaselinePublicationResult::TornStage) {
      const auto discarded = discardTorn(CourseBaselinePublicationPhase::Prepared);
      if (discarded != CourseBaselinePublicationResult::Ok) return finish(discarded);
    }
    if (publishedStage == CourseBaselinePublicationResult::TornStage) {
      const auto discarded = discardTorn(CourseBaselinePublicationPhase::Published);
      if (discarded != CourseBaselinePublicationResult::Ok) return finish(discarded);
    }
    auto result = save(CourseBaselinePublicationPhase::Prepared);
    if (result != CourseBaselinePublicationResult::Ok) return finish(result);
    if (!guard() || !active.publishArchive(active.context, expected))
      return finish(CourseBaselinePublicationResult::IoError);
    if (!guard() || !active.verifyPublished(active.context, expected))
      return finish(CourseBaselinePublicationResult::VerificationFailed);
    result = save(CourseBaselinePublicationPhase::Published);
    if (result != CourseBaselinePublicationResult::Ok) return finish(result);
    if (!guard() || !active.verifyPublished(active.context, expected))
      return finish(CourseBaselinePublicationResult::VerificationFailed);
    return finish(CourseBaselinePublicationResult::Ok);
  }
  const CourseBaselinePublicationRecord* published() const {
    if (!guard()) ready = false;
    return ready ? &expected : nullptr;
  }
  void close() { ready = false; }

 private:
  TransferStorage& storage;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  CourseBaselinePublicationRecord expected, observed;
  CourseBaselinePublicationHooks active;
  std::array<char, 96> prepared{}, preparedStage{}, completed{}, completedStage{};
  bool operating = false;
  mutable bool ready = false;
  bool guard() const { return permitted && permitted(context); }
  static bool available(CourseBaselinePublicationResult result) {
    return result == CourseBaselinePublicationResult::Ok || result == CourseBaselinePublicationResult::Missing ||
           result == CourseBaselinePublicationResult::TornStage;
  }
  const char* path(CourseBaselinePublicationPhase phase, bool staged) const {
    if (phase == CourseBaselinePublicationPhase::Prepared) return staged ? preparedStage.data() : prepared.data();
    return staged ? completedStage.data() : completed.data();
  }
  void paths() {
    static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-baseline-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(PREFIX.size() + 32 + sizeof(".published.tmp") <= 96);
    std::copy(PREFIX.begin(), PREFIX.end(), prepared.begin());
    size_t at = PREFIX.size();
    for (const auto byte : expected.request.transaction) {
      prepared[at++] = HEX_DIGITS[byte >> 4];
      prepared[at++] = HEX_DIGITS[byte & 15];
    }
    std::copy_n(prepared.begin(), at, preparedStage.begin());
    std::copy_n(prepared.begin(), at, completed.begin());
    std::copy_n(prepared.begin(), at, completedStage.begin());
    static constexpr char PREPARED[] = ".prepared", PREPARED_STAGE[] = ".prepared.tmp";
    static constexpr char PUBLISHED[] = ".published", PUBLISHED_STAGE[] = ".published.tmp";
    std::copy_n(PREPARED, sizeof(PREPARED), prepared.begin() + at);
    std::copy_n(PREPARED_STAGE, sizeof(PREPARED_STAGE), preparedStage.begin() + at);
    std::copy_n(PUBLISHED, sizeof(PUBLISHED), completed.begin() + at);
    std::copy_n(PUBLISHED_STAGE, sizeof(PUBLISHED_STAGE), completedStage.begin() + at);
  }
  CourseBaselinePublicationResult read(CourseBaselinePublicationPhase phase, bool staged) {
    if (!guard()) return CourseBaselinePublicationResult::Busy;
    uint64_t size = 0;
    const auto status = storage.stat(path(phase, staged), size);
    if (!guard()) return CourseBaselinePublicationResult::Busy;
    if (status == FileStatus::Missing) return CourseBaselinePublicationResult::Missing;
    if (status == FileStatus::Error) return CourseBaselinePublicationResult::IoError;
    if (size < COURSE_BASELINE_PUBLICATION_SIZE && staged) return inspectTorn(phase, size);
    if (size != COURSE_BASELINE_PUBLICATION_SIZE) return CourseBaselinePublicationResult::Corrupt;
    const auto bytes = scratch.first(COURSE_BASELINE_PUBLICATION_SIZE);
    if (!storage.read(path(phase, staged), 0, bytes)) return CourseBaselinePublicationResult::IoError;
    if (!guard()) return CourseBaselinePublicationResult::Busy;
    if (!decodeCourseBaselinePublicationRecord(bytes, observed)) return CourseBaselinePublicationResult::Corrupt;
    return observed.phase == phase && observed.reader == expected.reader && observed.request == expected.request
               ? CourseBaselinePublicationResult::Ok
               : CourseBaselinePublicationResult::Conflict;
  }
  CourseBaselinePublicationResult inspectTorn(CourseBaselinePublicationPhase phase, size_t length) {
    if (scratch.size() <= COURSE_BASELINE_PUBLICATION_SIZE) return CourseBaselinePublicationResult::Invalid;
    observed = expected;
    observed.phase = phase;
    const auto encoded = scratch.first(COURSE_BASELINE_PUBLICATION_SIZE);
    if (!encodeCourseBaselinePublicationRecord(observed, encoded)) return CourseBaselinePublicationResult::Invalid;
    const auto buffer = scratch.subspan(COURSE_BASELINE_PUBLICATION_SIZE);
    for (size_t offset = 0; offset < length;) {
      const auto count = std::min(buffer.size(), length - offset);
      if (!guard()) return CourseBaselinePublicationResult::Busy;
      if (!storage.read(path(phase, true), offset, buffer.first(count)))
        return CourseBaselinePublicationResult::IoError;
      if (!guard()) return CourseBaselinePublicationResult::Busy;
      if (!std::equal(buffer.begin(), buffer.begin() + count, encoded.begin() + offset))
        return CourseBaselinePublicationResult::Corrupt;
      offset += count;
    }
    return guard() ? CourseBaselinePublicationResult::TornStage : CourseBaselinePublicationResult::Busy;
  }
  CourseBaselinePublicationResult discardTorn(CourseBaselinePublicationPhase phase) {
    const auto inspected = read(phase, true);
    if (inspected != CourseBaselinePublicationResult::TornStage)
      return inspected == CourseBaselinePublicationResult::Ok || inspected == CourseBaselinePublicationResult::Missing
                 ? CourseBaselinePublicationResult::Conflict
                 : inspected;
    if (!guard()) return CourseBaselinePublicationResult::Busy;
    if (!storage.remove(path(phase, true))) return CourseBaselinePublicationResult::IoError;
    const auto remaining = read(phase, true);
    return remaining == CourseBaselinePublicationResult::Missing ? CourseBaselinePublicationResult::Ok
                                                                 : CourseBaselinePublicationResult::IoError;
  }
  CourseBaselinePublicationResult save(CourseBaselinePublicationPhase phase) {
    auto result = read(phase, false);
    if (result == CourseBaselinePublicationResult::Ok) {
      result = read(phase, true);
      return result == CourseBaselinePublicationResult::Missing ? CourseBaselinePublicationResult::Ok
             : result == CourseBaselinePublicationResult::Ok    ? CourseBaselinePublicationResult::Conflict
                                                                : result;
    }
    if (result != CourseBaselinePublicationResult::Missing) return result;
    result = read(phase, true);
    if (result == CourseBaselinePublicationResult::Missing) {
      expected.phase = phase;
      const bool encoded =
          encodeCourseBaselinePublicationRecord(expected, scratch.first(COURSE_BASELINE_PUBLICATION_SIZE));
      expected.phase = CourseBaselinePublicationPhase::Prepared;
      if (!encoded) return CourseBaselinePublicationResult::Invalid;
      if (!guard()) return CourseBaselinePublicationResult::Busy;
      if (!storage.write(path(phase, true), 0, scratch.first(COURSE_BASELINE_PUBLICATION_SIZE), true))
        return CourseBaselinePublicationResult::IoError;
      result = read(phase, true);
    }
    if (result != CourseBaselinePublicationResult::Ok) return result;
    uint64_t size = 0;
    if (!guard()) return CourseBaselinePublicationResult::Busy;
    const auto status = storage.stat(path(phase, false), size);
    if (status == FileStatus::Error) return CourseBaselinePublicationResult::IoError;
    if (status != FileStatus::Missing) return CourseBaselinePublicationResult::Conflict;
    if (!guard()) return CourseBaselinePublicationResult::Busy;
    if (!storage.rename(path(phase, true), path(phase, false))) return CourseBaselinePublicationResult::IoError;
    return read(phase, false);
  }
  CourseBaselinePublicationResult finish(CourseBaselinePublicationResult result) {
    if (!guard()) result = CourseBaselinePublicationResult::Busy;
    operating = false;
    ready = result == CourseBaselinePublicationResult::Ok;
    if (ready) expected.phase = CourseBaselinePublicationPhase::Published;
    return result;
  }
};
}  // namespace companion

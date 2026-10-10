#pragma once

#include "CompanionUnboundCourseMigrationIntentStore.h"
#include "CompanionUnboundCourseReviewReservation.h"

namespace companion {
// Retain off stack; caller lends checked storage/workspace and excludes writers.
// Verify authenticates exclusive Native epoch reservation and frozen counts.
class UnboundCourseReviewReservationStore final {
 public:
  using Permission = bool (*)(void*);
  using Verify = bool (*)(void*, const UnboundCourseReviewReservation&);
  inline static constexpr char PATH[] = "/.crosspoint/companion/course-unbound.review-reservation";
  inline static constexpr char STAGE[] = "/.crosspoint/companion/course-unbound.review-reservation.tmp";
  UnboundCourseReviewReservationStore(TransferStorage& storage, std::span<uint8_t> scratch, Permission permitted,
                                      void* context)
      : storage(storage), scratch(scratch), permitted(permitted), context(context) {}
  UnboundCourseReviewReservationStore(const UnboundCourseReviewReservationStore&) = delete;
  UnboundCourseReviewReservationStore& operator=(const UnboundCourseReviewReservationStore&) = delete;
  UnboundCourseIntentResult load(UnboundCourseReviewReservation& output) {
    if (operating) return UnboundCourseIntentResult::Busy;
    if (!arguments(&output, sizeof(output))) return UnboundCourseIntentResult::Invalid;
    operating = true;
    cancelled = false;
    const auto canonical = read(PATH);
    if (canonical != UnboundCourseIntentResult::Ok && canonical != UnboundCourseIntentResult::Missing)
      return finish(canonical);
    uint64_t size = 0;
    if (!guard()) return finish(UnboundCourseIntentResult::Busy);
    const auto stage = storage.stat(STAGE, size);
    if (!guard()) return finish(UnboundCourseIntentResult::Busy);
    if (stage == FileStatus::Error) return finish(UnboundCourseIntentResult::IoError);
    if (stage == FileStatus::Present) {
      if (canonical == UnboundCourseIntentResult::Ok) return finish(UnboundCourseIntentResult::Conflict);
      if (size < UNBOUND_COURSE_REVIEW_RESERVATION_SIZE) return finish(UnboundCourseIntentResult::Pending);
      const auto result = read(STAGE);
      return finish(result == UnboundCourseIntentResult::Ok ? UnboundCourseIntentResult::Pending : result);
    }
    const auto result = finish(canonical);
    if (result == UnboundCourseIntentResult::Ok) output = observed;
    return result;
  }
  UnboundCourseIntentResult persist(const UnboundCourseReviewReservation& input, Verify verify,
                                    void* verificationContext) {
    if (operating) return UnboundCourseIntentResult::Busy;
    if (!verify || !validUnboundCourseReviewReservation(input) || !arguments(&input, sizeof(input)))
      return UnboundCourseIntentResult::Invalid;
    expected = input;
    operating = true;
    cancelled = false;
    bool sealed = false, staged = false, torn = false;
    uint64_t length = 0;
    auto result = inspect(sealed, staged, torn, length);
    if (result != UnboundCourseIntentResult::Ok) return finish(result);
    if (!guard()) return finish(UnboundCourseIntentResult::Busy);
    if (!verify(verificationContext, expected)) return finish(UnboundCourseIntentResult::VerificationFailed);
    if (!guard()) return finish(UnboundCourseIntentResult::Busy);
    bool againSealed = false, againStaged = false, againTorn = false;
    uint64_t againLength = 0;
    result = inspect(againSealed, againStaged, againTorn, againLength);
    if (result != UnboundCourseIntentResult::Ok) return finish(result);
    if (sealed != againSealed || staged != againStaged || torn != againTorn || length != againLength)
      return finish(UnboundCourseIntentResult::Conflict);
    if (sealed) return finish(UnboundCourseIntentResult::Ok);
    if (torn) {
      if (!guard()) return finish(UnboundCourseIntentResult::Busy);
      if (!storage.remove(STAGE)) return finish(UnboundCourseIntentResult::IoError);
      uint64_t size = 0;
      if (!guard()) return finish(UnboundCourseIntentResult::Busy);
      if (storage.stat(STAGE, size) != FileStatus::Missing) return finish(UnboundCourseIntentResult::IoError);
      if (!guard()) return finish(UnboundCourseIntentResult::Busy);
    }
    if (!staged) {
      auto bytes = scratch.first(UNBOUND_COURSE_REVIEW_RESERVATION_SIZE);
      if (!encodeUnboundCourseReviewReservation(expected, bytes)) return finish(UnboundCourseIntentResult::Invalid);
      if (!guard()) return finish(UnboundCourseIntentResult::Busy);
      if (!storage.write(STAGE, 0, bytes, true)) return finish(UnboundCourseIntentResult::IoError);
    }
    result = read(STAGE);
    if (result != UnboundCourseIntentResult::Ok) return finish(result);
    if (observed != expected) return finish(UnboundCourseIntentResult::Conflict);
    uint64_t size = 0;
    if (!guard()) return finish(UnboundCourseIntentResult::Busy);
    const auto status = storage.stat(PATH, size);
    if (!guard()) return finish(UnboundCourseIntentResult::Busy);
    if (status == FileStatus::Error) return finish(UnboundCourseIntentResult::IoError);
    if (status != FileStatus::Missing) return finish(UnboundCourseIntentResult::Conflict);
    if (!storage.rename(STAGE, PATH)) return finish(UnboundCourseIntentResult::IoError);
    result = read(PATH);
    return finish(result == UnboundCourseIntentResult::Ok && observed != expected ? UnboundCourseIntentResult::Conflict
                                                                                  : result);
  }
  void close() {
    if (operating) cancelled = true;
  }

 private:
  TransferStorage& storage;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  UnboundCourseReviewReservation expected, observed;
  bool operating = false, cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled; }
  bool arguments(const void* value, size_t size) const {
    return scratch.size() > UNBOUND_COURSE_REVIEW_RESERVATION_SIZE &&
           !course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) &&
           !course_baseline_detail::overlaps(scratch.data(), scratch.size(), value, size) &&
           !course_baseline_detail::overlaps(this, sizeof(*this), value, size);
  }
  UnboundCourseIntentResult read(const char* path) {
    if (!guard()) return UnboundCourseIntentResult::Busy;
    uint64_t size = 0;
    const auto status = storage.stat(path, size);
    if (!guard()) return UnboundCourseIntentResult::Busy;
    if (status == FileStatus::Missing) return UnboundCourseIntentResult::Missing;
    if (status == FileStatus::Error) return UnboundCourseIntentResult::IoError;
    if (size != UNBOUND_COURSE_REVIEW_RESERVATION_SIZE) return UnboundCourseIntentResult::Corrupt;
    auto bytes = scratch.first(UNBOUND_COURSE_REVIEW_RESERVATION_SIZE);
    if (!storage.read(path, 0, bytes)) return UnboundCourseIntentResult::IoError;
    if (!guard()) return UnboundCourseIntentResult::Busy;
    return decodeUnboundCourseReviewReservation(bytes, observed) ? UnboundCourseIntentResult::Ok
                                                                 : UnboundCourseIntentResult::Corrupt;
  }
  UnboundCourseIntentResult prefix(uint64_t length) {
    auto bytes = scratch.first(UNBOUND_COURSE_REVIEW_RESERVATION_SIZE);
    if (!encodeUnboundCourseReviewReservation(expected, bytes)) return UnboundCourseIntentResult::Invalid;
    auto buffer = scratch.subspan(UNBOUND_COURSE_REVIEW_RESERVATION_SIZE);
    for (uint64_t offset = 0; offset < length;) {
      const auto count = std::min<uint64_t>(buffer.size(), length - offset);
      if (!guard()) return UnboundCourseIntentResult::Busy;
      if (!storage.read(STAGE, offset, buffer.first(count))) return UnboundCourseIntentResult::IoError;
      if (!guard()) return UnboundCourseIntentResult::Busy;
      if (!std::equal(buffer.begin(), buffer.begin() + count, bytes.begin() + offset))
        return UnboundCourseIntentResult::Corrupt;
      offset += count;
    }
    return UnboundCourseIntentResult::Ok;
  }
  UnboundCourseIntentResult inspect(bool& sealed, bool& staged, bool& torn, uint64_t& length) {
    const auto canonical = read(PATH);
    if (canonical != UnboundCourseIntentResult::Ok && canonical != UnboundCourseIntentResult::Missing) return canonical;
    sealed = canonical == UnboundCourseIntentResult::Ok;
    if (sealed && observed != expected) return UnboundCourseIntentResult::Conflict;
    if (!guard()) return UnboundCourseIntentResult::Busy;
    const auto status = storage.stat(STAGE, length);
    if (!guard()) return UnboundCourseIntentResult::Busy;
    if (status == FileStatus::Error) return UnboundCourseIntentResult::IoError;
    if (status == FileStatus::Missing) return UnboundCourseIntentResult::Ok;
    if (sealed) return UnboundCourseIntentResult::Conflict;
    if (length < UNBOUND_COURSE_REVIEW_RESERVATION_SIZE) {
      torn = true;
      return prefix(length);
    }
    const auto result = read(STAGE);
    if (result != UnboundCourseIntentResult::Ok) return result;
    if (observed != expected) return UnboundCourseIntentResult::Conflict;
    staged = true;
    return UnboundCourseIntentResult::Ok;
  }
  UnboundCourseIntentResult finish(UnboundCourseIntentResult result) {
    if (!guard()) result = UnboundCourseIntentResult::Busy;
    operating = false;
    return result;
  }
};
}  // namespace companion

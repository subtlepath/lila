#pragma once

#include "CompanionCourseBaselineImportRequest.h"
#include "CompanionTransfer.h"

namespace companion {
enum class CourseBaselineConsentResult { Ok, Missing, Invalid, Busy, Conflict, Corrupt, IoError };
// Retain off stack. Caller prepares parents, verifies the frozen review and exact
// authenticated transfer, and excludes writers. This saves consent only; it does
// not verify a pack, grant learner-state reuse or publish an archive.
class CourseBaselineImportConsent final {
 public:
  using Permission = bool (*)(void*);
  CourseBaselineImportConsent(TransferStorage& storage, std::span<uint8_t> scratch, Permission permitted, void* context)
      : storage(storage), scratch(scratch), permitted(permitted), context(context) {}
  CourseBaselineImportConsent(const CourseBaselineImportConsent&) = delete;
  CourseBaselineImportConsent& operator=(const CourseBaselineImportConsent&) = delete;
  CourseBaselineConsentResult load(const Identity& transaction, CourseBaselineImportRequest& output) {
    if (operating) return CourseBaselineConsentResult::Busy;
    if (!arguments(transaction) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &output, sizeof(output)))
      return CourseBaselineConsentResult::Invalid;
    operating = true;
    auto result = read(canonical.data());
    if (result == CourseBaselineConsentResult::Ok) {
      const auto staged = read(stage.data());
      result = staged == CourseBaselineConsentResult::Missing ? CourseBaselineConsentResult::Ok
               : staged == CourseBaselineConsentResult::Ok    ? CourseBaselineConsentResult::Conflict
                                                              : staged;
    }
    result = finish(result);
    if (result == CourseBaselineConsentResult::Ok) output = observed;
    return result;
  }
  CourseBaselineConsentResult persist(const CourseBaselineImportRequest& request, bool recoverTornStage = false) {
    if (operating) return CourseBaselineConsentResult::Busy;
    if (!validCourseBaselineImportRequest(request) || !arguments(request.transaction) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &request, sizeof(request)))
      return CourseBaselineConsentResult::Invalid;
    operating = true;
    auto result = read(canonical.data());
    if (result == CourseBaselineConsentResult::Ok) {
      if (observed != request) return finish(CourseBaselineConsentResult::Conflict);
      result = read(stage.data());
      return finish(result == CourseBaselineConsentResult::Missing ? CourseBaselineConsentResult::Ok
                    : result == CourseBaselineConsentResult::Ok    ? CourseBaselineConsentResult::Conflict
                                                                   : result);
    }
    if (result != CourseBaselineConsentResult::Missing) return finish(result);
    result = read(stage.data());
    if (result == CourseBaselineConsentResult::Corrupt && recoverTornStage) result = discardTornStage(request);
    if (result == CourseBaselineConsentResult::Ok) {
      if (observed != request) return finish(CourseBaselineConsentResult::Conflict);
    } else if (result == CourseBaselineConsentResult::Missing) {
      auto bytes = scratch.first(COURSE_BASELINE_IMPORT_REQUEST_SIZE);
      if (!encodeCourseBaselineImportRequest(request, bytes)) return finish(CourseBaselineConsentResult::Invalid);
      if (!guard()) return finish(CourseBaselineConsentResult::Busy);
      if (!storage.write(stage.data(), 0, bytes, true)) return finish(CourseBaselineConsentResult::IoError);
      result = read(stage.data());
      if (result != CourseBaselineConsentResult::Ok) return finish(result);
      if (observed != request) return finish(CourseBaselineConsentResult::Conflict);
    } else {
      return finish(result);
    }
    uint64_t size = 0;
    if (!guard()) return finish(CourseBaselineConsentResult::Busy);
    const auto status = storage.stat(canonical.data(), size);
    if (!guard()) return finish(CourseBaselineConsentResult::Busy);
    if (status == FileStatus::Error) return finish(CourseBaselineConsentResult::IoError);
    if (status != FileStatus::Missing) return finish(CourseBaselineConsentResult::Conflict);
    if (!storage.rename(stage.data(), canonical.data())) return finish(CourseBaselineConsentResult::IoError);
    result = read(canonical.data());
    return finish(result == CourseBaselineConsentResult::Ok && observed != request
                      ? CourseBaselineConsentResult::Conflict
                      : result);
  }

 private:
  TransferStorage& storage;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  std::array<char, 96> canonical{}, stage{};
  CourseBaselineImportRequest observed;
  bool operating = false;
  bool guard() const { return permitted && permitted(context); }
  bool arguments(const Identity& transaction) {
    if (scratch.size() < COURSE_BASELINE_IMPORT_REQUEST_SIZE ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        std::none_of(transaction.begin(), transaction.end(), [](uint8_t byte) { return byte != 0; }))
      return false;
    static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-baseline-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    static_assert(PREFIX.size() + 32 + sizeof(".consent.tmp") <= 96);
    size_t at = PREFIX.size();
    std::copy(PREFIX.begin(), PREFIX.end(), canonical.begin());
    for (const auto byte : transaction) {
      canonical[at++] = HEX_DIGITS[byte >> 4];
      canonical[at++] = HEX_DIGITS[byte & 15];
    }
    static constexpr char SUFFIX[] = ".consent";
    std::copy_n(SUFFIX, sizeof(SUFFIX), canonical.begin() + at);
    at += sizeof(SUFFIX) - 1;
    std::copy_n(canonical.begin(), at, stage.begin());
    static constexpr char STAGE_SUFFIX[] = ".tmp";
    std::copy_n(STAGE_SUFFIX, sizeof(STAGE_SUFFIX), stage.begin() + at);
    return true;
  }
  CourseBaselineConsentResult read(const char* path) {
    if (!guard()) return CourseBaselineConsentResult::Busy;
    uint64_t size = 0;
    const auto status = storage.stat(path, size);
    if (!guard()) return CourseBaselineConsentResult::Busy;
    if (status == FileStatus::Missing) return CourseBaselineConsentResult::Missing;
    if (status == FileStatus::Error) return CourseBaselineConsentResult::IoError;
    if (size != COURSE_BASELINE_IMPORT_REQUEST_SIZE) return CourseBaselineConsentResult::Corrupt;
    const auto bytes = scratch.first(COURSE_BASELINE_IMPORT_REQUEST_SIZE);
    if (!storage.read(path, 0, bytes)) return CourseBaselineConsentResult::IoError;
    if (!guard()) return CourseBaselineConsentResult::Busy;
    if (!decodeCourseBaselineImportRequest(bytes, observed)) return CourseBaselineConsentResult::Corrupt;
    // Reconstructing the filename binds decoded consent to its transaction.
    static constexpr size_t TRANSACTION_OFFSET = sizeof("/.crosspoint/companion/course-baseline-") - 1;
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    for (size_t at = 0; at < observed.transaction.size(); ++at)
      if (path[TRANSACTION_OFFSET + 2 * at] != HEX_DIGITS[observed.transaction[at] >> 4] ||
          path[TRANSACTION_OFFSET + 2 * at + 1] != HEX_DIGITS[observed.transaction[at] & 15])
        return CourseBaselineConsentResult::Corrupt;
    return CourseBaselineConsentResult::Ok;
  }
  // Only fresh approval with native review/backup checks may opt into recovery.
  CourseBaselineConsentResult discardTornStage(const CourseBaselineImportRequest& request) {
    if (scratch.size() <= COURSE_BASELINE_IMPORT_REQUEST_SIZE) return CourseBaselineConsentResult::Invalid;
    uint64_t length = 0;
    if (!guard()) return CourseBaselineConsentResult::Busy;
    if (storage.stat(stage.data(), length) != FileStatus::Present) return CourseBaselineConsentResult::IoError;
    if (!guard()) return CourseBaselineConsentResult::Busy;
    if (length >= COURSE_BASELINE_IMPORT_REQUEST_SIZE) return CourseBaselineConsentResult::Corrupt;
    const auto encoded = scratch.first(COURSE_BASELINE_IMPORT_REQUEST_SIZE);
    if (!encodeCourseBaselineImportRequest(request, encoded)) return CourseBaselineConsentResult::Invalid;
    const auto buffer = scratch.subspan(COURSE_BASELINE_IMPORT_REQUEST_SIZE);
    for (size_t offset = 0; offset < length;) {
      const auto count = std::min<uint64_t>(buffer.size(), length - offset);
      if (!guard()) return CourseBaselineConsentResult::Busy;
      if (!storage.read(stage.data(), offset, buffer.first(count))) return CourseBaselineConsentResult::IoError;
      if (!guard()) return CourseBaselineConsentResult::Busy;
      if (!std::equal(buffer.begin(), buffer.begin() + count, encoded.begin() + offset))
        return CourseBaselineConsentResult::Corrupt;
      offset += count;
    }
    uint64_t size = 0;
    if (!guard()) return CourseBaselineConsentResult::Busy;
    const auto canonicalStatus = storage.stat(canonical.data(), size);
    if (!guard()) return CourseBaselineConsentResult::Busy;
    if (canonicalStatus == FileStatus::Error) return CourseBaselineConsentResult::IoError;
    if (canonicalStatus != FileStatus::Missing) return CourseBaselineConsentResult::Conflict;
    if (!storage.remove(stage.data())) return CourseBaselineConsentResult::IoError;
    if (!guard()) return CourseBaselineConsentResult::Busy;
    const auto stageStatus = storage.stat(stage.data(), size);
    if (!guard()) return CourseBaselineConsentResult::Busy;
    return stageStatus == FileStatus::Missing ? CourseBaselineConsentResult::Missing
                                              : CourseBaselineConsentResult::IoError;
  }
  CourseBaselineConsentResult finish(CourseBaselineConsentResult result) {
    if (!guard()) result = CourseBaselineConsentResult::Busy;
    operating = false;
    return result;
  }
};
}  // namespace companion

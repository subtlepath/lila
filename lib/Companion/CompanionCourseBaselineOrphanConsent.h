#pragma once

#include "CompanionCourseBaselineImportConsent.h"

namespace companion {
// Retain off stack. Permission proves native transfer-journal inspection found
// no parent for this transaction and excludes state/namespace writers. Orphan
// bytes remain diagnostic evidence and never become canonical approval.
class CourseBaselineOrphanConsent final {
 public:
  using Permission = bool (*)(void*);
  CourseBaselineOrphanConsent(TransferStorage& storage, Permission permitted, void* context)
      : storage(storage), permitted(permitted), context(context) {}
  CourseBaselineOrphanConsent(const CourseBaselineOrphanConsent&) = delete;
  CourseBaselineOrphanConsent& operator=(const CourseBaselineOrphanConsent&) = delete;
  CourseBaselineConsentResult rollback(const Identity& transaction) {
    if (operating) return CourseBaselineConsentResult::Busy;
    if (transaction == Identity{}) return CourseBaselineConsentResult::Invalid;
    const auto selected = transaction;
    names(selected);
    operating = true;
    static constexpr std::string_view PROTECTED[] = {".consent", ".prepared", ".prepared.tmp", ".published",
                                                     ".published.tmp"};
    for (const auto suffix : PROTECTED) {
      std::copy(suffix.begin(), suffix.end(), probe.begin() + STEM_SIZE);
      probe[STEM_SIZE + suffix.size()] = 0;
      uint64_t size = 0;
      if (!guard()) return finish(CourseBaselineConsentResult::Busy);
      const auto status = storage.stat(probe.data(), size);
      if (!guard()) return finish(CourseBaselineConsentResult::Busy);
      if (status == FileStatus::Error) return finish(CourseBaselineConsentResult::IoError);
      if (status != FileStatus::Missing) return finish(CourseBaselineConsentResult::Conflict);
    }
    uint64_t stagedSize = 0, orphanSize = 0;
    if (!guard()) return finish(CourseBaselineConsentResult::Busy);
    const auto staged = storage.stat(stage.data(), stagedSize);
    if (!guard()) return finish(CourseBaselineConsentResult::Busy);
    const auto orphaned = storage.stat(orphan.data(), orphanSize);
    if (!guard()) return finish(CourseBaselineConsentResult::Busy);
    if (staged == FileStatus::Error || orphaned == FileStatus::Error)
      return finish(CourseBaselineConsentResult::IoError);
    if ((staged == FileStatus::Present && stagedSize > COURSE_BASELINE_IMPORT_REQUEST_SIZE) ||
        (orphaned == FileStatus::Present && orphanSize > COURSE_BASELINE_IMPORT_REQUEST_SIZE))
      return finish(CourseBaselineConsentResult::Corrupt);
    if (staged == FileStatus::Missing)
      return finish(orphaned == FileStatus::Present ? CourseBaselineConsentResult::Ok
                                                    : CourseBaselineConsentResult::Missing);
    if (orphaned != FileStatus::Missing) {
      bool available = false;
      for (unsigned slot = 0; slot < ORPHAN_SLOTS; ++slot) {
        numberedOrphan(slot);
        if (!guard()) return finish(CourseBaselineConsentResult::Busy);
        const auto status = storage.stat(orphan.data(), orphanSize);
        if (!guard()) return finish(CourseBaselineConsentResult::Busy);
        if (status == FileStatus::Error) return finish(CourseBaselineConsentResult::IoError);
        if (status == FileStatus::Missing) {
          available = true;
          break;
        }
        if (orphanSize > COURSE_BASELINE_IMPORT_REQUEST_SIZE) return finish(CourseBaselineConsentResult::Corrupt);
      }
      if (!available) return finish(CourseBaselineConsentResult::Conflict);
    }
    if (!storage.rename(stage.data(), orphan.data())) return finish(CourseBaselineConsentResult::IoError);
    if (!guard()) return finish(CourseBaselineConsentResult::Busy);
    const auto remaining = storage.stat(stage.data(), orphanSize);
    if (!guard()) return finish(CourseBaselineConsentResult::Busy);
    const auto preserved = storage.stat(orphan.data(), orphanSize);
    return finish(remaining == FileStatus::Missing && preserved == FileStatus::Present && orphanSize == stagedSize
                      ? CourseBaselineConsentResult::Ok
                      : CourseBaselineConsentResult::IoError);
  }

 private:
  static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-baseline-";
  static constexpr size_t STEM_SIZE = PREFIX.size() + 32;
  static constexpr unsigned ORPHAN_SLOTS = 256;
  static_assert(STEM_SIZE + sizeof(".consent.orphan-ff") <= 96);
  TransferStorage& storage;
  Permission permitted;
  void* context;
  std::array<char, 96> stage{}, orphan{}, probe{};
  bool operating = false;
  bool guard() const { return permitted && permitted(context); }
  void names(const Identity& transaction) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::copy(PREFIX.begin(), PREFIX.end(), stage.begin());
    size_t at = PREFIX.size();
    for (const auto byte : transaction) {
      stage[at++] = DIGITS[byte >> 4];
      stage[at++] = DIGITS[byte & 15];
    }
    std::copy_n(stage.begin(), STEM_SIZE, orphan.begin());
    std::copy_n(stage.begin(), STEM_SIZE, probe.begin());
    static constexpr char STAGE_SUFFIX[] = ".consent.tmp", ORPHAN_SUFFIX[] = ".consent.orphan";
    std::copy_n(STAGE_SUFFIX, sizeof(STAGE_SUFFIX), stage.begin() + STEM_SIZE);
    std::copy_n(ORPHAN_SUFFIX, sizeof(ORPHAN_SUFFIX), orphan.begin() + STEM_SIZE);
  }
  void numberedOrphan(unsigned slot) {
    static constexpr char SUFFIX[] = ".consent.orphan-", DIGITS[] = "0123456789abcdef";
    std::copy_n(SUFFIX, sizeof(SUFFIX) - 1, orphan.begin() + STEM_SIZE);
    constexpr size_t AT = STEM_SIZE + sizeof(SUFFIX) - 1;
    orphan[AT] = DIGITS[slot >> 4];
    orphan[AT + 1] = DIGITS[slot & 15];
    orphan[AT + 2] = 0;
  }
  CourseBaselineConsentResult finish(CourseBaselineConsentResult result) {
    if (!guard()) result = CourseBaselineConsentResult::Busy;
    operating = false;
    return result;
  }
};
}  // namespace companion

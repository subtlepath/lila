#pragma once

#include "CompanionStoredCourseContinuity.h"
#include "HalCompanionHeapAdmission.h"
#include "HalCoursePackArchive.h"

namespace companion {
// Caller validates the installed catalog and excludes content/history writers.
// Retain fixed archive handles and manifest banks off stack; borrow recovery scratch.
class HalTintaPackTransition final {
 public:
  explicit HalTintaPackTransition(std::span<uint8_t> scratch)
      : scratch(scratch), storage(permitted, nullptr), archive(scratch, permitted, nullptr) {}
  bool verify(const Identity& course, const Digest& previous, const Digest& installed) {
    bool present = false;
    if (!permitted(nullptr) ||
        readCourseBinding(storage, COURSE_BINDING_PATH, scratch, binding, present) != CourseBindingResult::Ok ||
        !present || binding.logicalIdentity != course || binding.contentHash != installed ||
        !verifyInstalled(installed) || archive.open(course, previous) != CourseArchiveResult::Ok ||
        !archive.manifest() || !archive.path() || archive.manifest()->logicalIdentity != course ||
        archive.manifest()->formatVersion != binding.formatVersion)
      return failure("pack binding or archive");
    constexpr auto LOCALE_OFFSET = offsetof(tinta::core::pack::Header, locale);
    if (!storage.read(archive.path(), LOCALE_OFFSET, oldLocale) ||
        !storage.read(ACTIVE_COURSE_PATH, LOCALE_OFFSET, newLocale))
      return failure("locale read");
    for (size_t at = 0; at < oldLocale.size(); ++at)
      if (lower(oldLocale[at]) != lower(newLocale[at])) return failure("locale continuity");
    const auto continuity =
        compareStoredCourseItemIdentities(storage, archive.path(), ACTIVE_COURSE_PATH, yield, scratch);
    if (continuity != CourseItemContinuity::Compatible &&
        !(continuity == CourseItemContinuity::MissingHistory &&
          sameLegacyCourseRecords(storage, archive.path(), ACTIVE_COURSE_PATH, scratch, yield)))
      return failure("item continuity");
    return (archive.closeReaders() && permitted(nullptr)) || failure("archive close");
  }

 private:
  bool verifyInstalled(const Digest& expected) {
    uint64_t size = 0;
    Digest actual{};
    if (!permitted(nullptr) || storage.stat(ACTIVE_COURSE_PATH, size) != FileStatus::Present ||
        size != binding.length || !Storage.openFileForReadReusing("COMPANION", ACTIVE_COURSE_PATH, file))
      return failure("installed hash open");
    const bool hashed = !file.isDirectory() && hashInventoryFile(file, scratch, size, actual, permitted, nullptr);
    const bool synced = hashed && file.sync();
    const bool closed = file.close();
    return (hashed && synced && closed && size == binding.length && actual == expected && permitted(nullptr)) ||
           failure("installed hash/close");
  }
  static bool permitted(void*) { return Storage.ready() && admitCompanionHeap(); }
  static void yield() { vTaskDelay(1); }
  static uint8_t lower(uint8_t value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Tinta pack transition failed: %s", reason);
    return false;
  }
  std::span<uint8_t> scratch;
  HalCourseRemovalMetadata storage;
  HalCoursePackArchive archive;
  HalFile file;
  ContentManifest binding;
  std::array<uint8_t, 8> oldLocale{}, newLocale{};
};
}  // namespace companion

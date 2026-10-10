#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include "../Companion/CompanionCourseUidLookup.h"
#include "../Companion/CompanionTintaLegacySession.h"

namespace companion {
struct LegacySessionReport {
  uint32_t journalCount = 0;
  uint16_t queued = 0, mapped = 0, retired = 0, day = 0;
  bool journalChanged = false, hasSnapshot = false;
};
// Borrowed immutable hash-verified file/pack. Workspace holds the complete native
// saved file; caller supplies native screen/depth/queue limits and excludes writers.
inline bool inspectTintaLegacySession(HalFile& file, CourseUidLookup& items, uint32_t committedRecords,
                                      uint32_t lessonCount, uint32_t categoryCount, uint8_t maxDepth,
                                      uint8_t screenCount, uint16_t maxQueued, std::span<uint8_t> scratch,
                                      LegacySessionReport& output, bool (*permitted)(void*) = nullptr,
                                      void* context = nullptr) {
  const auto failure = []([[maybe_unused]] const char* reason) {
    LOG_ERR("COMPANION", "Legacy saved session inspection failed: %s", reason);
    return false;
  };
  if (!file.isOpen() || file.isDirectory() || !items.valid() || (permitted && !permitted(context)))
    return failure("arguments");
  const auto length = file.fileSize64();
  if (length > scratch.size() || length > UINT32_MAX || !file.seek64(0) ||
      file.read(scratch.data(), length) != static_cast<int64_t>(length))
    return failure("extent or read");
  LegacySessionView saved;
  if (!decodeTintaLegacySession(scratch.first(length), maxDepth, screenCount, maxQueued, saved))
    return failure("format");
  if (saved.kind == 1 && saved.tag &&
      ((saved.tag & 0x8000u) ? (saved.tag & 0x7fffu) >= categoryCount : saved.tag - 1u >= lessonCount))
    return failure("practice target");
  LegacySessionReport report;
  report.journalCount = saved.journalCount;
  report.queued = saved.queued;
  report.day = saved.day;
  report.journalChanged = !saved.file.session.empty() && saved.journalCount != committedRecords;
  report.hasSnapshot = !saved.file.snapshot.empty();
  for (size_t at = 0; at < saved.entries.size(); at += 5) {
    int32_t index = -1;
    if ((permitted && !permitted(context)) || !items.find(binary_record::getU32(saved.entries.data() + at), index))
      return failure("item read or cancellation");
    if (index < 0)
      ++report.retired;
    else
      ++report.mapped;
  }
  if (file.fileSize64() != length || !items.valid() || (permitted && !permitted(context)))
    return failure("extent or cancellation");
  output = report;
  return true;
}
}  // namespace companion

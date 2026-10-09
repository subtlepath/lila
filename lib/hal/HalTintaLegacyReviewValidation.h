#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include "../Companion/CompanionCourseUidLookup.h"
#include "../Companion/CompanionLegacyTintaJournal.h"

namespace companion {
struct LegacyReviewReport {
  uint32_t records = 0, mapped = 0, retired = 0, zeroTailBytes = 0;
};
// The caller verifies immutable reviewed file and pack hashes and excludes all
// writers. This checks legacy syntax/undo references, not distributed provenance
// or replay equivalence with the retained items snapshot.
inline bool inspectTintaLegacyReviews(HalFile& file, CourseUidLookup& catalog, uint32_t committedRecords,
                                      std::span<uint8_t> scratch, LegacyReviewReport& output,
                                      bool (*permitted)(void*) = nullptr, void* context = nullptr) {
  const auto failure = []([[maybe_unused]] const char* reason) {
    LOG_ERR("COMPANION", "Legacy review inspection failed: %s", reason);
    return false;
  };
  if (!file.isOpen() || file.isDirectory() || !catalog.valid() || scratch.size() < 12 ||
      (permitted && !permitted(context)))
    return failure("arguments");
  const auto length = file.fileSize64();
  if (length > LegacyTintaJournalDecoder::MAX_BYTES || !file.seek64(0)) return failure("extent");
  LegacyTintaJournalDecoder decoder;
  LegacyReviewReport result;
  for (uint64_t offset = 0; offset < length;) {
    if (permitted && !permitted(context)) return failure("cancelled");
    const auto count = std::min<uint64_t>(12, length - offset);
    if (file.read(scratch.data(), count) != static_cast<int>(count)) return failure("read");
    LegacyTintaEntry entry;
    const auto decoded = decoder.next(scratch.first(count), entry);
    if (decoded == LegacyTintaDecodeResult::ZeroTail) {
      result.zeroTailBytes += count;
    } else if (decoded == LegacyTintaDecodeResult::Record) {
      int32_t index = -1;
      if (!catalog.find(entry.uid, index)) return failure("catalog read");
      if (index < 0)
        ++result.retired;
      else
        ++result.mapped;
    } else {
      return failure("record");
    }
    offset += count;
  }
  result.records = decoder.count();
  if (result.records < committedRecords || file.fileSize64() != length || !catalog.valid() ||
      (permitted && !permitted(context)))
    return failure("committed count, extent or cancellation");
  output = result;
  return true;
}
}  // namespace companion

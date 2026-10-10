#pragma once

#if LILA_TINTA

#include "HalCompanionHeapAdmission.h"
#include "HalCourseRemovalMetadata.h"
#include "HalJournalCausalAuditSession.h"

namespace companion {
// Caller excludes all journal writers. Release each fixed reader before the
// next allocation; these owners exceed the native stack budget.
inline bool prepareHalCourseBaselineJournal(HalCourseRemovalMetadata::Permission permitted, void* context) {
  const auto guard = [&]() { return permitted && permitted(context) && Storage.ready(); };
  if (!guard()) {
    LOG_ERR("COMPANION", "Baseline journal permission unavailable");
    return false;
  }
  if (!admitCompanionHeap(sizeof(HalCompanionFileLookup), sizeof(HalCompanionFileLookup))) return false;
  auto lookup = makeUniqueNoThrow<HalCompanionFileLookup>();
  if (!lookup) {
    LOG_ERR("COMPANION", "OOM: baseline journal presence reader");
    return false;
  }
  const auto presence = lookup->inspect(TINTA_JOURNAL_DIRECTORY);
  lookup.reset();
  if (!guard() || presence == CompanionFilePresence::Error) {
    LOG_ERR("COMPANION", "Baseline journal presence unavailable");
    return false;
  }
  if (presence == CompanionFilePresence::Missing) return true;
  if (!admitCompanionHeap(sizeof(HalCourseRemovalMetadata), sizeof(HalCourseRemovalMetadata))) return false;
  auto metadata = makeUniqueNoThrow<HalCourseRemovalMetadata>(permitted, context);
  if (!metadata) {
    LOG_ERR("COMPANION", "OOM: baseline journal metadata reader");
    return false;
  }
  uint64_t size = 0;
  const auto events = metadata->stat(TINTA_JOURNAL_EVENTS, size);
  const auto a = metadata->stat(TINTA_JOURNAL_HEADER_A, size);
  const auto b = metadata->stat(TINTA_JOURNAL_HEADER_B, size);
  const bool metadataClosed = metadata->closeReaders();
  metadata.reset();
  if (!metadataClosed || !guard() || events != FileStatus::Present || a == FileStatus::Error ||
      b == FileStatus::Error || (a != FileStatus::Present && b != FileStatus::Present)) {
    LOG_ERR("COMPANION", "Baseline journal evidence incomplete");
    return false;
  }
  if (!admitCompanionHeap(sizeof(HalJournalCausalAuditSession), sizeof(HalJournalCausalAuditSession))) return false;
  auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
  if (!audit) {
    LOG_ERR("COMPANION", "OOM: baseline journal audit");
    return false;
  }
  if (!audit->run() || !guard()) {
    LOG_ERR("COMPANION", "Baseline journal audit unavailable");
    return false;
  }
  return true;
}
}  // namespace companion

#endif

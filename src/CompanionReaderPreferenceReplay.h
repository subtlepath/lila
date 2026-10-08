#pragma once

#include <Memory.h>

#include "CompanionReaderPreferenceMetadata.h"
#include "CompanionReaderPreferenceStore.h"
#include "HalJournalCausalAuditSession.h"

namespace companion {
// Retain off-stack. The caller finishes publication recovery, unbinds local
// capture, excludes journal/content/settings writers and replays on startup.
class NativeReaderPreferenceReplay final {
 public:
  NativeReaderPreferenceReplay(CrossPointSettings& settings, NativeReaderPreferenceMetadata& metadata)
      : metadata(metadata), store(settings, &proveAuthority, this) {}
  ReaderPreferenceApplicationResult run() {
    proofReady = false;
    resolution.clear();
    // Audit/index buffers exceed the task stack and are released before the
    // content-validation phase; no allocation occurs in its record loop.
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run(&frontier)) return failure("authority audit");
    count = audit->recordCount();
    recordSize = audit->recordSize();
    const auto resolved = audit->resolvePortablePreferences(resolution);
    audit.reset();
    if (resolved == TintaJournalResult::Conflict) return ReaderPreferenceApplicationResult::Conflict;
    if (resolved != TintaJournalResult::Ok) return failure("preference resolution");
    proofReady = true;
    auto result = application.run(resolution.bodies(), metadata.dependencies(), store);
    if (result == ReaderPreferenceApplicationResult::Unchanged && !proveAuthority(this, {}))
      result = ReaderPreferenceApplicationResult::IoError;
    proofReady = false;
    return result;
  }

 private:
  static bool proveAuthority(void* context, const ReaderPreferenceValues&) {
    auto& owner = *static_cast<NativeReaderPreferenceReplay*>(context);
    if (!owner.proofReady) return false;
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit || !audit->run(&owner.rechecked) || owner.rechecked != owner.frontier ||
        audit->recordCount() != owner.count || audit->recordSize() != owner.recordSize) {
      failure("authority changed before settings application");
      return false;
    }
    return true;
  }
  static ReaderPreferenceApplicationResult failure(const char* reason) {
    LOG_ERR("COMPANION", "Reader preference replay failed: %s", reason);
    return ReaderPreferenceApplicationResult::IoError;
  }
  NativeReaderPreferenceMetadata& metadata;
  NativeReaderPreferenceStore store;
  PortablePreferenceResolution resolution;
  ReaderPreferenceApplication application;
  Digest frontier{}, rechecked{};
  uint32_t count = 0;
  uint16_t recordSize = 0;
  bool proofReady = false;
};
}  // namespace companion

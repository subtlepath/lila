#pragma once

#include "HalRemovalMetadataSnapshotStorage.h"

namespace companion {
// The immutable declaration and journal are loaded before binding a publisher.
// The enclosing owner excludes declaration/metadata writers until publication
// finishes; this cached proof avoids SD lookup on every streamed hash callback.
class HalRemovalMetadataAuthorization final {
 public:
  HalRemovalMetadataAuthorization(ContentRemovalJournal& journal, HalRemovalMetadataSnapshotStorage& storage)
      : journal(journal), storage(storage) {}
  bool bind(const RemovalMetadataSnapshot& expected) {
    ready = false;
    const auto* current = journal.current();
    if (!validRemovalMetadataSnapshot(expected) || !current || current->request != expected.request ||
        current->planHash != expected.planHash ||
        (current->phase != ContentRemovalPhase::Quarantined && current->phase != ContentRemovalPhase::Committed &&
         current->phase != ContentRemovalPhase::Retired))
      return failure("journal ownership");
    checkpoint = *current;
    if (storage.load(expected.planHash, expected.file, proof) != RemovalMetadataStorageResult::Ok ||
        proof != expected || !journal.current() || *journal.current() != checkpoint)
      return failure("durable declaration");
    ready = true;
    return true;
  }
  static bool check(void* context, const RemovalMetadataSnapshot& expected) {
    if (!context) return false;
    const auto& self = *static_cast<HalRemovalMetadataAuthorization*>(context);
    return self.ready && expected == self.proof && self.journal.current() && *self.journal.current() == self.checkpoint;
  }

 private:
  ContentRemovalJournal& journal;
  HalRemovalMetadataSnapshotStorage& storage;
  RemovalMetadataSnapshot proof;
  ContentRemovalRecord checkpoint;
  bool ready = false;
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Removal metadata authorization failed: %s", reason);
    return false;
  }
};
}  // namespace companion

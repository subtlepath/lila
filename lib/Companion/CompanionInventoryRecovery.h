#pragma once
#include "CompanionInventoryRollback.h"
namespace companion {
class InventoryRecovery {
 public:
  InventoryRecovery(TransferStorage& storage, InventoryPublication& publication, InventoryRollback& rollback)
      : storage(storage), publication(publication), rollback(rollback) {}
  InventoryPublicationResult recover(const Identity& generation) {
    if (!storage.prepare()) return InventoryPublicationResult::IoError;
    bool pending = false;
    if (!rollback.pending(pending)) return InventoryPublicationResult::IoError;
    if (pending) {
      bool valid = false;
      const auto checked = rollback.hasValidIntent(generation, valid);
      if (checked != InventoryPublicationResult::Ok) return checked;
      if (valid) {
        const auto restored = rollback.recover(generation);
        if (restored != InventoryPublicationResult::Ok) return restored;
      } else {
        return fallback(generation);
      }
    }
    const auto result = publication.recover(generation);
    return result == InventoryPublicationResult::Corrupt ? fallback(generation) : result;
  }

 private:
  TransferStorage& storage;
  InventoryPublication& publication;
  InventoryRollback& rollback;
  InventoryPublicationResult fallback(const Identity& generation) {
    uint64_t revision = 0;
    const auto checked = publication.pendingRevision(generation, revision);
    if (checked != InventoryPublicationResult::Ok) return checked;
    if (revision == 0) return InventoryPublicationResult::Corrupt;
    auto result = rollback.begin(generation, revision);
    if (result == InventoryPublicationResult::Corrupt) result = rollback.beginEmpty(generation, revision);
    if (result != InventoryPublicationResult::Ok) return result;
    return publication.recover(generation);
  }
};
}  // namespace companion

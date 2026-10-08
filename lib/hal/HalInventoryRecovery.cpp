#include "HalInventoryRecovery.h"

#include <Logging.h>
#include <Memory.h>

#include "CompanionInventoryRecovery.h"
#include "HalInventoryPublicationValidator.h"
namespace companion {
namespace {
struct RecoveryState {
  HalInventoryPublicationValidator validator;
  InventoryPublication publication;
  InventoryRollback rollback;
  InventoryRecovery recovery;
  RecoveryState(TransferStorage& storage, std::span<uint8_t> scratch)
      : validator(scratch),
        publication(storage, validator, scratch),
        rollback(storage, validator, scratch),
        recovery(storage, publication, rollback) {}
};
}  // namespace
InventoryPublicationResult recoverInventorySnapshots(TransferStorage& storage, const Identity& generation,
                                                     std::span<uint8_t> scratch) {
  if (scratch.size() <= INVENTORY_PATH_MAX_RECORD || !inventory_detail::nonzero(generation)) {
    LOG_ERR("COMPANION", "Invalid inventory recovery workspace or generation");
    return InventoryPublicationResult::Invalid;
  }
  // Reader/controller state exceeds the task-local budget; scratch stays borrowed.
  auto state = makeUniqueNoThrow<RecoveryState>(storage, scratch);
  if (!state) {
    LOG_ERR("COMPANION", "OOM: inventory recovery state");
    return InventoryPublicationResult::IoError;
  }
  const auto result = state->recovery.recover(generation);
  if (result != InventoryPublicationResult::Ok)
    LOG_ERR("COMPANION", "Inventory recovery failed: %u", static_cast<unsigned>(result));
  return result;
}
}  // namespace companion

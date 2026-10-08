#pragma once

#include <Logging.h>

#include <algorithm>

#include "CompanionInventoryBuild.h"
#include "HalInventoryFileSource.h"
#include "HalInventoryIndexStage.h"
#include "HalInventoryPathsStage.h"
#include "HalInventoryPublicationValidator.h"
#include "HalInventoryRevisionAllocator.h"
#include "HalInventorySortStorage.h"

namespace companion {
// Session-owned outside the task stack. Storage, resolver and workspace outlive it.
class HalInventoryBuildSession final {
 public:
  static constexpr size_t WORKSPACE_SIZE = 8192;
  static constexpr size_t HASH_SIZE = 4096;
  HalInventoryBuildSession(TransferStorage& storage, InventoryFileResolver& resolver, std::span<uint8_t> workspace)
      : workspace(workspace),
        validator(workspace),
        publication(storage, validator, workspace),
        rollback(storage, validator, workspace),
        recovery(storage, publication, rollback),
        paths(pathStage, partition(workspace, HASH_SIZE, INVENTORY_PATH_MAX_RECORD)),
        sorter(runs, partition(workspace, HASH_SIZE + INVENTORY_PATH_MAX_RECORD, WORKSPACE_SIZE)),
        index(indexStage, partition(workspace, HASH_SIZE + INVENTORY_PATH_MAX_RECORD, WORKSPACE_SIZE)),
        source(walker, resolver, paths, partition(workspace, 0, HASH_SIZE)),
        builder(storage, validator, revisions, recovery, publication, paths, pathStage, sorter, index, indexStage) {}
  HalInventoryBuildSession(const HalInventoryBuildSession&) = delete;
  HalInventoryBuildSession& operator=(const HalInventoryBuildSession&) = delete;
  InventoryPublicationResult build(const Identity& generation, uint64_t& revision) {
    if (workspace.size() < WORKSPACE_SIZE) {
      LOG_ERR("COMPANION", "Inventory build requires an 8 KiB workspace");
      return InventoryPublicationResult::Invalid;
    }
    const auto result = builder.build(source, generation, revision);
    if (result != InventoryPublicationResult::Ok)
      LOG_ERR("COMPANION", "Inventory build failed: %u", static_cast<unsigned>(result));
    return result;
  }

 private:
  static std::span<uint8_t> partition(std::span<uint8_t> bytes, size_t offset, size_t count) {
    if (offset >= bytes.size()) return {};
    return bytes.subspan(offset, std::min(count, bytes.size() - offset));
  }
  std::span<uint8_t> workspace;
  HalInventoryPublicationValidator validator;
  InventoryPublication publication;
  InventoryRollback rollback;
  InventoryRecovery recovery;
  HalInventoryRevisionAllocator revisions;
  HalInventoryPathsStage pathStage;
  InventoryPathsBuilder paths;
  HalInventorySortStorage runs;
  InventorySorter sorter;
  HalInventoryIndexStage indexStage;
  InventoryIndexBuilder index;
  HalInventoryDirectoryWalker walker;
  HalInventoryFileSource source;
  InventoryBuild builder;
};
}  // namespace companion

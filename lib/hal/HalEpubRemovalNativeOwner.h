#pragma once

#include "HalEpubRemovalSession.h"
#include "HalInventoryPublicationValidator.h"

namespace companion {
// Retained off-stack owner. Callbacks supply authenticated writer exclusion,
// lazy inventory preparation and native state/inventory refresh.
class HalEpubRemovalNativeOwner final {
 public:
  HalEpubRemovalNativeOwner(const Identity& generation, HalEpubRemovalBackend::Callback permitted,
                            HalEpubRemovalBackend::Callback refresh, void* context,
                            HalEpubRemovalAdmission::InventoryReady inventoryReady,
                            FontRemovalSettings* fontSettings = nullptr)
      : generation(generation),
        validator(scratch),
        paths(pathStorage, std::span(scratch).first(INVENTORY_PATH_MAX_RECORD)),
        session(generation, paths, revision, MULTI_PATH_REMOVAL_HEADER_SIZE, permitted, refresh, context,
                inventoryReady, fontSettings) {}
  bool prepare() { return session.prepare(); }
  bool openInventory(uint64_t requestedRevision) {
    if (!requestedRevision ||
        validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation, requestedRevision) !=
            InventoryValidation::Valid ||
        !pathStorage.open(InventoryPublication::PATHS))
      return failure("inventory pair");
    uint64_t bytes = 0;
    if (!pathStorage.size(bytes) || bytes > UINT64_MAX - MULTI_PATH_REMOVAL_HEADER_SIZE ||
        !session.setPlanQuota(bytes + MULTI_PATH_REMOVAL_HEADER_SIZE))
      return failure("plan quota");
    revision = requestedRevision;
    return true;
  }
  bool closeReaders() {
    const bool planClosed = session.closeReaders();
    const bool pathsClosed = pathStorage.close();
    return planClosed && pathsClosed;
  }
  size_t handle(bool authorized, const Identity& owner, std::span<const uint8_t> request, std::span<uint8_t> reply) {
    return session.handle(authorized, owner, request, reply);
  }

 private:
  Identity generation;
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD + 128> scratch{};
  HalInventoryPublicationValidator validator;
  HalInventoryIndexStorage pathStorage;
  InventoryPaths paths;
  uint64_t revision = 0;
  HalEpubRemovalSession session;
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Native EPUB removal owner %s failed", reason);
    return false;
  }
};
}  // namespace companion

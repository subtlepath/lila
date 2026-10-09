#pragma once

#include "HalDictionaryRemovalSession.h"
#include "HalInventoryPublicationValidator.h"

namespace companion {
// Retain once after checked heap admission. Fixed proof/decoder state exceeds
// task-stack limits; inventory validation and hashing reuse one IO workspace.
class HalDictionaryRemovalNativeOwner final {
 public:
  HalDictionaryRemovalNativeOwner(const Identity& generation, uint64_t maximumPlanBytes,
                                  DictionaryRemovalSettings& settings, HalEpubRemovalBackend::Callback permitted,
                                  HalEpubRemovalBackend::Callback refresh, void* context,
                                  HalEpubRemovalAdmission::InventoryReady inventoryReady)
      : generation(generation),
        maximumPlanBytes(maximumPlanBytes),
        validator(scratch),
        paths(pathStorage, std::span(scratch).first(INVENTORY_PATH_MAX_RECORD)),
        session(generation, paths, revision, maximumPlanBytes, scratch, settings, permitted, refresh, context,
                inventoryReady) {}
  bool prepare() { return session.prepare(); }
  bool openInventory(uint64_t requestedRevision) {
    if (!requestedRevision ||
        validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation, requestedRevision) !=
            InventoryValidation::Valid ||
        !pathStorage.open(InventoryPublication::PATHS))
      return failure("inventory pair");
    InventoryIndexHeader header;
    if (!pathStorage.read(0, std::span(scratch).first(INVENTORY_INDEX_HEADER_SIZE)) ||
        !decodeInventoryPathsHeader(std::span(scratch).first(INVENTORY_INDEX_HEADER_SIZE), header) ||
        header.generation != generation || header.revision != requestedRevision ||
        header.count > (UINT64_MAX - DICTIONARY_REMOVAL_COHORT_HEADER_SIZE) / DICTIONARY_REMOVAL_PLAN_SIZE)
      return failure("inventory quota header");
    const uint64_t required = DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + header.count * DICTIONARY_REMOVAL_PLAN_SIZE;
    if (!session.setPlanQuota(std::min(maximumPlanBytes, required))) return failure("plan quota");
    revision = requestedRevision;
    return true;
  }
  bool setPlanQuota(uint64_t bytes) {
    if (!session.setPlanQuota(bytes)) return false;
    maximumPlanBytes = bytes;
    return true;
  }
  bool closeReaders() {
    const bool sessionClosed = session.closeReaders();
    const bool pathsClosed = pathStorage.close();
    return sessionClosed && pathsClosed;
  }
  size_t handle(bool authorized, const Identity& owner, std::span<const uint8_t> request, std::span<uint8_t> reply) {
    return session.handle(authorized, owner, request, reply);
  }

 private:
  Identity generation;
  uint64_t maximumPlanBytes;
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD + DICTIONARY_BINDING_SIZE> scratch{};
  HalInventoryPublicationValidator validator;
  HalInventoryIndexStorage pathStorage;
  InventoryPaths paths;
  uint64_t revision = 0;
  HalDictionaryRemovalSession session;
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Native dictionary removal owner %s failed", reason);
    return false;
  }
};
}  // namespace companion

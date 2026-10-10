#include "HalCompanionRecovery.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <array>

#include "CompanionFirmwareInstallIntent.h"
#include "CompanionFrame.h"
#include "CompanionInventoryRollback.h"
#include "HalContentRemovalStartupRecovery.h"
#include "HalIdentityStorage.h"
#include "HalInventoryRecovery.h"
#include "HalJournalStartupRecovery.h"
#include "HalTransferStorage.h"
#if LILA_TINTA
#include "HalCourseBaselineRecovery.h"
#include "HalPairingsStorage.h"
#endif

namespace companion {
namespace {
struct RemovalRecovery {
  RemovalRecovery(FontRemovalSettings* settings, DictionaryRemovalSettings* dictionarySettings)
      : removal(settings, dictionarySettings) {}
  HalContentRemovalStartupRecovery removal;
  HalIdentityStorage identityStorage;
  IdentityState identity;
};
struct JournalRecovery {
  HalJournalStartupRecovery journal;
  HalIdentityStorage identities;
};
struct Recovery {
  std::array<uint8_t, SESSION_WORKSPACE_SIZE> workspace;
  HalIdentityStorage identityStorage;
  IdentityState identity;
  HalTransferStorage storage;
  Transfer transfer{storage, workspace};
  std::unique_ptr<HalDictionaryTransferInstaller> dictionaryInstaller;
#if LILA_TINTA
  HalPairingsStorage pairingsStorage;
  Pairings pairings{pairingsStorage};
  std::unique_ptr<HalCourseBaselineImportSession> baselineSession;
  bool baselineWorkspaceOwned = false;
  static bool baselinePermission(void* context) {
    return static_cast<Recovery*>(context)->baselineWorkspaceOwned && Storage.ready();
  }
#endif
  ~Recovery() {
    storage.setDictionaryInstaller(nullptr);
    storage.setCourseBaselineInstaller(nullptr);
  }
  FirmwareInstallAuthorization firmwareAuthorization;
  Digest runningFirmwareHash{};
};
bool recoverFirmwareIntent(Recovery& recovery, bool (*runningDigest)(std::span<uint8_t>, std::span<uint8_t>)) {
  FirmwareInstallIntent intent(recovery.storage, recovery.workspace);
  const auto result = intent.load(recovery.firmwareAuthorization);
  uint64_t size = 0;
  const auto stage = recovery.storage.stat(FIRMWARE_INSTALL_INTENT_STAGE, size);
  if (result == FirmwareInstallIntentResult::Missing && stage == FileStatus::Missing) return true;
  if (result != FirmwareInstallIntentResult::Ok || stage != FileStatus::Missing || !runningDigest ||
      recovery.firmwareAuthorization.request.generation != recovery.identity.storageGeneration) {
    LOG_ERR("COMPANION", "Firmware intent recovery unavailable or ambiguous");
    return false;
  }
  if (!runningDigest(recovery.workspace, recovery.runningFirmwareHash)) {
    LOG_ERR("COMPANION", "Running firmware digest unavailable; intent retained");
    return false;
  }
  if (recovery.runningFirmwareHash != recovery.firmwareAuthorization.request.hash) {
    LOG_INF("COMPANION", "Firmware installation has not booted; authenticated retry available");
    return true;
  }
  if (intent.retireVerifiedImage(recovery.identity.storageGeneration, recovery.runningFirmwareHash) !=
      FirmwareInstallIntentResult::Ok) {
    LOG_ERR("COMPANION", "Verified firmware intent retirement failed");
    return false;
  }
  LOG_INF("COMPANION", "Firmware boot verified; installation intent retired");
  return true;
}
}  // namespace
bool recoverAtStartup(bool (*firmwareValidator)(const char*),
                      bool (*runningDigest)(std::span<uint8_t>, std::span<uint8_t>), FontRemovalSettings* fontSettings,
                      DictionaryRemovalSettings* dictionarySettings) {
  if (!Storage.ready()) {
    LOG_ERR("COMPANION", "Startup recovery requires storage");
    return false;
  }
  {
    // Journal records and lookup handles exceed the boot stack budget. Release
    // them before removal/transfer workspaces and reader stores are allocated.
    auto recovery = makeUniqueNoThrow<JournalRecovery>();
    if (!recovery) {
      LOG_ERR("COMPANION", "OOM: journal startup recovery workspace");
      return false;
    }
    if (!recovery->journal.run(recovery->identities)) return false;
  }
  {
    // Removal buffers/participants exceed the boot stack. Release this owner
    // before allocating the transfer workspace or loading reader stores/fonts.
    auto recovery = makeUniqueNoThrow<RemovalRecovery>(fontSettings, dictionarySettings);
    if (!recovery) {
      LOG_ERR("COMPANION", "OOM: removal startup recovery workspace");
      return false;
    }
    bool removalPending = false;
    if (!recovery->removal.pending(removalPending)) return false;
    if (removalPending) {
      if (provisionIdentity(recovery->identityStorage, recovery->identity) != IdentityResult::Ok) {
        LOG_ERR("COMPANION", "Removal startup identity failed");
        return false;
      }
      if (!recovery->removal.run(recovery->identity.storageGeneration)) return false;
    }
  }
  bool orphanConsentPending = false;
#if LILA_TINTA
  if (!hasHalCourseBaselineOrphanConsents(orphanConsentPending, [](void*) { return Storage.ready(); }, nullptr))
    return false;
#endif
  bool pending = orphanConsentPending || Storage.exists(TRANSFER_JOURNALS[0]) || Storage.exists(TRANSFER_JOURNALS[1]) ||
                 Storage.exists(TRANSFER_STAGE) || Storage.exists(TRANSFER_BACKUP) ||
                 Storage.exists(FIRMWARE_INSTALL_INTENT_PATH) || Storage.exists(FIRMWARE_INSTALL_INTENT_STAGE);
  static constexpr const char* INVENTORY_FILES[] = {InventoryPublication::INDEX,      InventoryPublication::PATHS,
                                                    InventoryPublication::INDEX_NEXT, InventoryPublication::PATHS_NEXT,
                                                    InventoryPublication::INDEX_OLD,  InventoryPublication::PATHS_OLD,
                                                    InventoryPublication::INTENTS[0], InventoryPublication::INTENTS[1],
                                                    InventoryRollback::INTENTS[0],    InventoryRollback::INTENTS[1]};
  for (const char* path : INVENTORY_FILES) {
    if (Storage.exists(path)) {
      pending = true;
      break;
    }
  }
  if (!pending) return true;
  // Recovery needs the same 8 KiB scratch space as sync. Heap lifetime ends
  // before fonts and activities load; a local object exceeds the stack budget.
  auto recovery = makeUniqueNoThrow<Recovery>();
  if (!recovery) {
    LOG_ERR("COMPANION", "OOM: startup recovery workspace");
    return false;
  }
  if (provisionIdentity(recovery->identityStorage, recovery->identity) != IdentityResult::Ok) {
    LOG_ERR("COMPANION", "Startup recovery identity failed");
    return false;
  }
  recovery->dictionaryInstaller = createHalDictionaryTransferInstaller(
      recovery->transfer, recovery->identity.storageGeneration, recovery->workspace);
  if (!recovery->dictionaryInstaller) {
    LOG_ERR("COMPANION", "OOM: startup dictionary installer");
    return false;
  }
  recovery->storage.setDictionaryInstaller(recovery->dictionaryInstaller.get());
  recovery->storage.setFirmwareValidator(firmwareValidator);
#if LILA_TINTA
  recovery->baselineWorkspaceOwned = true;
  if (!attachHalCourseBaselineRecovery(recovery->transfer, recovery->storage, recovery->identityStorage,
                                       recovery->identity, recovery->pairings, recovery->workspace, recovery->workspace,
                                       recovery->baselineSession, Recovery::baselinePermission, recovery.get()))
    return false;
#endif
  const auto result = recovery->transfer.recover(recovery->identity.storageGeneration);
#if LILA_TINTA
  recovery->baselineWorkspaceOwned = false;
  recovery->storage.setCourseBaselineInstaller(nullptr);
  recovery->baselineSession.reset();
#endif
  if (result != TransferResult::Ok) {
    LOG_ERR("COMPANION", "Startup recovery failed: %u", static_cast<unsigned>(result));
    return false;
  }
  if (recoverInventorySnapshots(recovery->storage, recovery->identity.storageGeneration, recovery->workspace) !=
      InventoryPublicationResult::Ok)
    return false;
  return recoverFirmwareIntent(*recovery, runningDigest);
}
}  // namespace companion

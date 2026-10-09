#include "CompanionConnectActivity.h"

#include <CompanionCapabilities.h>
#include <CompanionCommandQueue.h>
#include <CompanionFrame.h>
#include <CompanionInventoryHandler.h>
#include <CompanionRecords.h>
#include <CompanionTransferHandler.h>
#include <CompanionWorkspace.h>
#include <FontCacheManager.h>
#include <HalCompanionWifiMessages.h>
#include <HalInventoryRecovery.h>
#include <HalInventoryResolverSession.h>
#include <HalJournalFormatInventory.h>
#include <HalJournalMergeStartupRecovery.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include "CompanionCourseSwitchHandler.h"
#include "CompanionFirmwareReaderInfo.h"
#include "CompanionLegacyCourseStateMigration.h"
#include "CompanionReaderPreferences.h"
#include "SdCardFontSystem.h"
#include "network/FirmwareFlasher.h"
#if LILA_TINTA
#include <HalTintaJournalMergeCommitContext.h>
#include <HalTintaMergedJournalReconciliation.h>
#endif
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointState.h"
#include "HalJournalStateQuery.h"
#include "RecentBooksStore.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"

namespace {
size_t journalFormatReply(std::span<uint8_t> payload) {
  if (payload.size() < 2) {
    LOG_ERR("COMPANION", "Journal format reply buffer too small");
    return 0;
  }
  payload[0] = 1;
  // Lookup buffers exceed the task stack; allocate once for this read-only query.
  auto formats = makeUniqueNoThrow<companion::HalJournalFormatInventory>();
  uint8_t versions = 0;
  if (!formats || !formats->inspect(versions)) {
    LOG_ERR("COMPANION", "Journal format query unavailable");
    return 1;
  }
  payload[0] = 0;
  payload[1] = versions;
  return 2;
}
struct InventoryScanSession {
  std::array<uint8_t, 32768> window;
  tinfl_decompressor decoder;
  companion::HalInventoryResolverSession resolver;
  companion::HalInventoryBuildSession builder;
  InventoryScanSession(companion::TransferStorage& storage, std::span<uint8_t> workspace)
      : resolver(storage, workspace, decoder, window), builder(storage, resolver, workspace) {}
};
constexpr size_t FRAME_SIZE = companion::FRAME_BUFFER_SIZE;
constexpr size_t OUTPUT_START = companion::REQUEST_OFFSET;
static_assert(OUTPUT_START + 2 * FRAME_SIZE <= companion::SESSION_WORKSPACE_SIZE);

void logInventoryHeap(const char* phase) {
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 1
  const auto heap = HalMemory::getInternalHeap();
  LOG_INF("COMPANION", "Inventory heap %s: free=%u largest=%u boot-min=%u", phase,
          static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlockBytes),
          static_cast<unsigned>(heap.minFreeBytes));
#else
  (void)phase;
#endif
}

[[gnu::noinline]] size_t inventoryReply(companion::InventoryCatalog& catalog, bool authorized,
                                        std::span<const uint8_t> request, std::span<uint8_t> response) {
  return companion::handleInventory(catalog, authorized, request, response);
}

companion::Board board() {
#if FREEINK_DEVICE_STICKY
  return companion::Board::Sticky;
#elif FREEINK_DEVICE_X4PRO
  return companion::Board::X4Pro;
#elif FREEINK_DEVICE_X4CLASSIC
  return companion::Board::X4Classic;
#elif FREEINK_DEVICE_PAPERMONO
  return companion::Board::PaperMono;
#else
  return companion::Board::X4;
#endif
}
}  // namespace

void CompanionConnectActivity::onEnter() {
  companion::suspendReaderPreferenceCapture();
  // Hold the render lock until callbacks and session storage are ready.
  RenderLock lock;
  Activity::onEnter();
  requestUpdate();
  sdFontSystem.releaseForTransfer(renderer);
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  // One reusable session buffer; a stack buffer would exceed the C3 stack budget.
  workspace = makeUniqueNoThrow<uint8_t[]>(companion::SESSION_WORKSPACE_SIZE);
  if (!workspace) {
    LOG_ERR("COMPANION", "OOM: 8 KiB workspace");
    return;
  }
  if (companion::provisionIdentity(identityStorage, identity) != companion::IdentityResult::Ok) {
    LOG_ERR("COMPANION", "Identity provisioning failed");
    return;
  }
  transferStorage.setFirmwareValidator(firmware_flash::validateForNextPartition);
  transfer.emplace(transferStorage,
                   std::span(workspace.get(), companion::SESSION_WORKSPACE_SIZE).subspan(companion::TRANSFER_OFFSET));
  dictionaryInstaller = companion::createHalDictionaryTransferInstaller(
      *transfer, identity.storageGeneration,
      std::span(workspace.get(), companion::SESSION_WORKSPACE_SIZE).subspan(companion::TRANSFER_OFFSET));
  if (!dictionaryInstaller) {
    recoveryBlocked = true;
    return;
  }
  transferStorage.setDictionaryInstaller(dictionaryInstaller.get());
  if (!companion::recoverExistingJournalMergeAbort(identity.storageGeneration)) {
    recoveryBlocked = true;
    return;
  }
  const auto recovered = transfer->recover(identity.storageGeneration);
  if (recovered != companion::TransferResult::Ok) {
    recoveryBlocked = true;
    LOG_ERR("COMPANION", "Transfer recovery failed: %u", static_cast<unsigned>(recovered));
    return;
  }
  {
    // Publication records and retained lookup handles exceed the task stack budget.
    auto journalRecovery = makeUniqueNoThrow<companion::HalJournalMergeStartupRecovery>();
    if (!journalRecovery || !journalRecovery->run(identity.storageGeneration)) {
      LOG_ERR("COMPANION", "Journal publication startup recovery failed");
      recoveryBlocked = true;
      return;
    }
  }
#if LILA_TINTA
  {
    companion::Identity course{};
    bool present = false;
    auto context = makeUniqueNoThrow<companion::HalTintaJournalMergeCommitContext>();
    if (!context || !context->installedCourse(course, present)) {
      LOG_ERR("COMPANION", "Cannot resolve startup learner state");
      recoveryBlocked = true;
      return;
    }
    context.reset();
    if (present) {
      if (!companion::recoverBoundTintaDerivedPublication(course)) {
        LOG_ERR("COMPANION", "Learner publication startup recovery failed");
        recoveryBlocked = true;
        return;
      }
      // Release pack/lookup storage before allocating replay and publication buffers.
      auto reconciliation = makeUniqueNoThrow<companion::HalTintaMergedJournalReconciliation>(course, identityStorage);
      if (!reconciliation || !reconciliation->run()) {
        LOG_ERR("COMPANION", "Learner merge startup reconciliation failed");
        recoveryBlocked = true;
        return;
      }
      reconciliation.reset();
      auto localHistory = makeUniqueNoThrow<companion::HalTintaJournalMergeCommitContext>();
      if (!localHistory || !localHistory->reconcileLocalHistory(course, identity.storageGeneration, identityStorage)) {
        LOG_ERR("COMPANION", "Local learner history reconciliation failed");
        recoveryBlocked = true;
        return;
      }
    }
  }
#endif
  const auto inventoryRecovered = companion::recoverInventorySnapshots(
      transferStorage, identity.storageGeneration, {workspace.get(), companion::SESSION_WORKSPACE_SIZE});
  if (inventoryRecovered != companion::InventoryPublicationResult::Ok) {
    recoveryBlocked = true;
    return;
  }
  pairingsAvailable =
      pairings.load({workspace.get(), companion::SESSION_WORKSPACE_SIZE}) == companion::PairingResult::Ok;
  if (!pairingsAvailable) LOG_ERR("COMPANION", "Installation bindings unavailable");
  recoveryBlocked = false;
  if (!Storage.ensureDirectoryExists(companion::EPUB_DIRECTORY)) {
    LOG_ERR("COMPANION", "EPUB destination directory unavailable");
    return;
  }
  inventoryPending = true;
  requestUpdate();
}

bool CompanionConnectActivity::prepareInventory() {
  logInventoryHeap("before allocation");
  LOG_DBG("COMPANION", "Inventory scan owner: %u bytes", static_cast<unsigned>(sizeof(InventoryScanSession)));
  auto scan = makeUniqueNoThrow<InventoryScanSession>(transferStorage,
                                                      std::span(workspace.get(), companion::SESSION_WORKSPACE_SIZE));
  if (!scan) {
    LOG_ERR("COMPANION", "OOM: inventory scan session");
    return false;
  }
  static constexpr size_t MIN_FREE_HEAP = 50 * 1024;
  logInventoryHeap("allocated");
  if (HalMemory::getInternalHeap().freeBytes <= MIN_FREE_HEAP) {
    LOG_ERR("COMPANION", "Insufficient heap for inventory scan");
    return false;
  }
  uint64_t revision = 0;
  if (!scan->resolver.prepareAfterRecovery() ||
      scan->builder.build(identity.storageGeneration, revision) != companion::InventoryPublicationResult::Ok) {
    logInventoryHeap("scan failed");
    return false;
  }
  logInventoryHeap("scan complete");
  scan.reset();
  logInventoryHeap("released");
  if (!inventoryStorage.open(companion::InventoryPublication::INDEX)) return false;
  inventory.emplace(inventoryStorage,
                    std::span(workspace.get(), companion::SESSION_WORKSPACE_SIZE).subspan(companion::TRANSFER_OFFSET));
  if (!inventory->open(identity.storageGeneration)) {
    LOG_ERR("COMPANION", "Published inventory cannot be opened");
    inventory.reset();
    inventoryStorage.close();
    return false;
  }
  return true;
}

bool CompanionConnectActivity::removalPermitted() const {
  if (!ready || !workspace || recoveryBlocked || firmwareInstallPending || inventoryPending || journalReceive ||
      legacyBackup || journalExportReady || !transfer)
    return false;
  const auto* current = transfer->current();
  return !current || current->phase == companion::TransferPhase::Committed ||
         current->phase == companion::TransferPhase::Aborted;
}
bool CompanionConnectActivity::contentReadPermitted() const {
  if ((!ready && wifiPhase != WifiPhase::Serving) || !workspace || recoveryBlocked || firmwareInstallPending ||
      inventoryPending || journalReceive || legacyBackup || journalExportReady || !transfer)
    return false;
  const auto* current = transfer->current();
  return !current || current->phase == companion::TransferPhase::Committed ||
         current->phase == companion::TransferPhase::Aborted;
}
bool CompanionConnectActivity::closeContentReaders() {
  exportTransaction = {};
  if (!contentReader || contentReader->closeReaders()) return true;
  recoveryBlocked = true;
  LOG_ERR("COMPANION", "Content export readers could not close");
  return false;
}
size_t CompanionConnectActivity::contentReadReply(bool authorized, std::span<const uint8_t> input,
                                                  std::span<uint8_t> output) {
  companion::ContentReadRequest request;
  if (!companion::decodeContentReadRequest(input, request)) return 0;
  auto result = companion::ContentReadResult::Unauthorized;
  if (!authorized) return companion::encodeContentReadReply(request, result, {}, output);
  result = companion::ContentReadResult::WrongStorage;
  if (request.generation != identity.storageGeneration)
    return companion::encodeContentReadReply(request, result, {}, output);
  result = companion::ContentReadResult::Busy;
  if (!contentReader || !contentReadPermitted() || !inventory || !inventory->revision())
    return companion::encodeContentReadReply(request, result, {}, output);
  uint64_t bytes = 0;
  for (const char* path : {companion::FIRMWARE_INSTALL_INTENT_PATH, companion::FIRMWARE_INSTALL_INTENT_STAGE}) {
    const auto status = transferStorage.stat(path, bytes);
    if (status == companion::FileStatus::Present) return companion::encodeContentReadReply(request, result, {}, output);
    if (status == companion::FileStatus::Error)
      return companion::encodeContentReadReply(request, companion::ContentReadResult::IoError, {}, output);
  }
  if (!contentReader->open(identity.storageGeneration, inventory->revision()))
    return companion::encodeContentReadReply(request, companion::ContentReadResult::IoError, {}, output);
  return contentReader->reply(installation, identity.storageGeneration, inventory->revision(), input, output);
}

size_t CompanionConnectActivity::contentMetadataReply(bool authorized, std::span<const uint8_t> input,
                                                      std::span<uint8_t> output) {
  companion::ContentMetadataRequest request;
  if (!companion::decodeContentMetadataRequest(input, request)) return 0;
  auto result = companion::ContentReadResult::Unauthorized;
  if (!authorized) return companion::encodeContentMetadataReply(request, result, {}, output);
  result = companion::ContentReadResult::WrongStorage;
  if (request.generation != identity.storageGeneration)
    return companion::encodeContentMetadataReply(request, result, {}, output);
  result = companion::ContentReadResult::Busy;
  if (!contentReader || !contentReadPermitted() || !inventory || !inventory->revision())
    return companion::encodeContentMetadataReply(request, result, {}, output);
  uint64_t bytes = 0;
  for (const char* path : {companion::FIRMWARE_INSTALL_INTENT_PATH, companion::FIRMWARE_INSTALL_INTENT_STAGE}) {
    const auto status = transferStorage.stat(path, bytes);
    if (status == companion::FileStatus::Present)
      return companion::encodeContentMetadataReply(request, result, {}, output);
    if (status == companion::FileStatus::Error)
      return companion::encodeContentMetadataReply(request, companion::ContentReadResult::IoError, {}, output);
  }
  if (!contentReader->open(identity.storageGeneration, inventory->revision()))
    return companion::encodeContentMetadataReply(request, companion::ContentReadResult::IoError, {}, output);
  return contentReader->metadataReply(request, identity.storageGeneration, inventory->revision(), output);
}

size_t CompanionConnectActivity::contentHandoffReply(bool authorized, std::span<const uint8_t> input,
                                                     std::span<uint8_t> output) {
  companion::ContentHandoffRequest request;
  if (!companion::decodeContentHandoffRequest(input, request)) return 0;
  if (!authorized)
    return companion::encodeContentHandoffReply(request, companion::ContentReadResult::Unauthorized, output);
  if (request.read.generation != identity.storageGeneration)
    return companion::encodeContentHandoffReply(request, companion::ContentReadResult::WrongStorage, output);
  if (!contentReader || !contentReadPermitted() || !inventory || !inventory->revision() ||
      wifiPhase != WifiPhase::None || wifiLease.phase() != companion::WifiHandoffLeasePhase::Empty)
    return companion::encodeContentHandoffReply(request, companion::ContentReadResult::Busy, output);
  uint64_t bytes = 0;
  for (const char* path : {companion::FIRMWARE_INSTALL_INTENT_PATH, companion::FIRMWARE_INSTALL_INTENT_STAGE}) {
    const auto status = transferStorage.stat(path, bytes);
    if (status == companion::FileStatus::Present)
      return companion::encodeContentHandoffReply(request, companion::ContentReadResult::Busy, output);
    if (status == companion::FileStatus::Error)
      return companion::encodeContentHandoffReply(request, companion::ContentReadResult::IoError, output);
  }
  if (!contentReader->open(identity.storageGeneration, inventory->revision()))
    return companion::encodeContentHandoffReply(request, companion::ContentReadResult::IoError, output);
  const auto length =
      contentReader->admitExport(request, installation, identity.storageGeneration, inventory->revision(), output);
  if (length == companion::CONTENT_HANDOFF_REPLY_SIZE &&
      output[4] == static_cast<uint8_t>(companion::ContentReadResult::Ok))
    exportTransaction = request.transaction;
  return length;
}

size_t CompanionConnectActivity::removalReply(bool authorized, const companion::Identity& owner,
                                              std::span<const uint8_t> body, std::span<uint8_t> reply) {
  if (reply.size() < companion::CONTENT_REMOVAL_REPLY_SIZE) return 0;
  std::fill_n(reply.begin(), companion::CONTENT_REMOVAL_REPLY_SIZE, 0);
  reply[0] = static_cast<uint8_t>(companion::ContentRemovalResult::Unauthorized);
  if (!authorized) return companion::CONTENT_REMOVAL_REPLY_SIZE;
  companion::ContentRemovalRequest request;
  if (!companion::multi_path_removal_detail::decodeRequest(body, request)) {
    reply[0] = static_cast<uint8_t>(companion::ContentRemovalResult::Invalid);
    return companion::CONTENT_REMOVAL_REPLY_SIZE;
  }
  std::copy(request.transaction.begin(), request.transaction.end(), reply.begin() + 1);
  if (request.owner != owner) return companion::CONTENT_REMOVAL_REPLY_SIZE;
  reply[0] = static_cast<uint8_t>(companion::ContentRemovalResult::WrongStorage);
  if (request.generation != identity.storageGeneration) return companion::CONTENT_REMOVAL_REPLY_SIZE;
  reply[0] = static_cast<uint8_t>(companion::ContentRemovalResult::Unsupported);
  if (request.manifest.kind != companion::ContentKind::Epub && request.manifest.kind != companion::ContentKind::Font &&
      request.manifest.kind != companion::ContentKind::Dictionary)
    return companion::CONTENT_REMOVAL_REPLY_SIZE;
  reply[0] = static_cast<uint8_t>(companion::ContentRemovalResult::Busy);
  if (!removalPermitted()) return companion::CONTENT_REMOVAL_REPLY_SIZE;
  if (!closeContentReaders()) return companion::CONTENT_REMOVAL_REPLY_SIZE;
  uint64_t bytes = 0;
  for (const char* path : {companion::FIRMWARE_INSTALL_INTENT_PATH, companion::FIRMWARE_INSTALL_INTENT_STAGE}) {
    const auto status = transferStorage.stat(path, bytes);
    if (status == companion::FileStatus::Present) return companion::CONTENT_REMOVAL_REPLY_SIZE;
    if (status == companion::FileStatus::Error) {
      reply[0] = static_cast<uint8_t>(companion::ContentRemovalResult::IoError);
      return companion::CONTENT_REMOVAL_REPLY_SIZE;
    }
  }
  reply[0] = static_cast<uint8_t>(companion::ContentRemovalResult::IoError);
  if (request.manifest.kind == companion::ContentKind::Dictionary) return dictionaryRemovalReply(owner, body, reply);
  if (dictionaryRemovalOwner) {
    if (!dictionaryRemovalOwner->closeReaders()) {
      recoveryBlocked = true;
      return companion::CONTENT_REMOVAL_REPLY_SIZE;
    }
    dictionaryRemovalOwner.reset();
  }
  if (!removalOwner) {
    if (!companion::admitCompanionHeap(sizeof(companion::HalEpubRemovalNativeOwner),
                                       sizeof(companion::HalEpubRemovalNativeOwner)))
      return companion::CONTENT_REMOVAL_REPLY_SIZE;
    // Retain fixed buffers/handles off stack and reuse for this connection.
    removalOwner = makeUniqueNoThrow<companion::HalEpubRemovalNativeOwner>(
        identity.storageGeneration,
        [](void* opaque) { return static_cast<CompanionConnectActivity*>(opaque)->removalPermitted(); },
        [](void* opaque) {
          auto& activity = *static_cast<CompanionConnectActivity*>(opaque);
          if (!activity.removalOwner->closeReaders()) {
            activity.recoveryBlocked = true;
            return false;
          }
          return activity.refreshAfterRemoval();
        },
        this,
        [](void* opaque, uint64_t& revision) {
          auto& activity = *static_cast<CompanionConnectActivity*>(opaque);
          if (!activity.inventory || !activity.inventory->revision()) {
            activity.inventory.reset();
            if (!activity.inventoryStorage.close() || !activity.prepareInventory()) return false;
          }
          revision = activity.inventory->revision();
          return activity.removalOwner->openInventory(revision);
        },
        &fontRemovalSettings);
    if (!removalOwner || !companion::admitCompanionHeap() || !removalOwner->prepare()) {
      LOG_ERR("COMPANION", "Removal owner allocation/heap admission failed");
      removalOwner.reset();
      return companion::CONTENT_REMOVAL_REPLY_SIZE;
    }
  }
  removalActive = true;
  const auto length = removalOwner->handle(true, owner, body, reply);
  removalActive = false;
  if (length && (reply[0] == static_cast<uint8_t>(companion::ContentRemovalResult::IoError) ||
                 reply[0] == static_cast<uint8_t>(companion::ContentRemovalResult::Corrupt)))
    recoveryBlocked = true;
  return length;
}

size_t CompanionConnectActivity::dictionaryRemovalReply(const companion::Identity& owner, std::span<const uint8_t> body,
                                                        std::span<uint8_t> reply) {
  if (removalOwner) {
    if (!removalOwner->closeReaders()) {
      recoveryBlocked = true;
      return companion::CONTENT_REMOVAL_REPLY_SIZE;
    }
    removalOwner.reset();
  }
  if (!dictionaryRemovalOwner) {
    if (!companion::admitCompanionHeap(sizeof(companion::HalDictionaryRemovalNativeOwner),
                                       sizeof(companion::HalDictionaryRemovalNativeOwner)))
      return companion::CONTENT_REMOVAL_REPLY_SIZE;
    dictionaryRemovalOwner = makeUniqueNoThrow<companion::HalDictionaryRemovalNativeOwner>(
        identity.storageGeneration, UINT64_MAX, dictionaryRemovalSettings,
        [](void* opaque) { return static_cast<CompanionConnectActivity*>(opaque)->removalPermitted(); },
        [](void* opaque) {
          auto& activity = *static_cast<CompanionConnectActivity*>(opaque);
          if (!activity.dictionaryRemovalOwner->closeReaders()) {
            activity.recoveryBlocked = true;
            return false;
          }
          return activity.refreshAfterRemoval();
        },
        this,
        [](void* opaque, uint64_t& revision) {
          auto& activity = *static_cast<CompanionConnectActivity*>(opaque);
          if (!activity.inventory || !activity.inventory->revision()) {
            activity.inventory.reset();
            if (!activity.inventoryStorage.close() || !activity.prepareInventory()) return false;
          }
          revision = activity.inventory->revision();
          return activity.dictionaryRemovalOwner->openInventory(revision);
        });
    if (!dictionaryRemovalOwner || !companion::admitCompanionHeap() || !dictionaryRemovalOwner->prepare()) {
      LOG_ERR("COMPANION", "Dictionary removal owner allocation/heap admission failed");
      dictionaryRemovalOwner.reset();
      return companion::CONTENT_REMOVAL_REPLY_SIZE;
    }
  }
  removalActive = true;
  const auto length = dictionaryRemovalOwner->handle(true, owner, body, reply);
  removalActive = false;
  if (length && (reply[0] == static_cast<uint8_t>(companion::ContentRemovalResult::IoError) ||
                 reply[0] == static_cast<uint8_t>(companion::ContentRemovalResult::Corrupt)))
    recoveryBlocked = true;
  return length;
}

bool CompanionConnectActivity::refreshAfterRemoval() {
  sdFontSystem.markRegistryDirty();
  inventory.reset();
  inventoryPending = true;
  if (!workspace || !inventoryStorage.close() || !APP_STATE.loadFromFile() || !RECENT_BOOKS.loadFromFile() ||
      !prepareInventory()) {
    LOG_ERR("COMPANION", "Reader state/inventory refresh after removal failed");
    inventoryPending = false;
    recoveryBlocked = true;
    requestUpdate();
    return false;
  }
  inventoryPending = false;
  requestUpdate();
  return true;
}

void CompanionConnectActivity::clearWifiMaterial() {
  companion::clearWifiHandoffSecrets(wifiMaterial.offer.session, wifiMaterial.offer.key);
  volatile char* password = wifiPassword.data();
  for (size_t at = 0; at < wifiPassword.size(); ++at) password[at] = 0;
  wifiSsid.fill(0);
  wifiMaterial = {};
  wifiReceivedAt = 0;
}
bool CompanionConnectActivity::journalHandoffMatches(const companion::Identity& transaction) const {
  const auto* merge = journalReceive ? journalReceive->receivingDeclaration() : nullptr;
  if (!merge || !companion::validJournalMergeIntent(*merge) || merge->transaction != transaction ||
      merge->owner != installation || merge->generation != identity.storageGeneration)
    return false;
  const auto count = journalReceive->count();
  return count >= merge->previous.count && count < merge->merged.count &&
         static_cast<uint64_t>(merge->merged.count - count) * merge->merged.recordSize > 1024 * 1024;
}
size_t CompanionConnectActivity::prepareWifi(const companion::WifiHandoffPrepare& request, uint64_t session,
                                             std::span<uint8_t> reply) {
  const auto* current = transfer ? transfer->current() : nullptr;
  const bool journal = journalHandoffMatches(request.transaction);
  const bool exporting = contentReader && inventory &&
                         contentReader->exportBinding().boundTo(request.transaction, installation,
                                                                identity.storageGeneration, inventory->revision());
  if (recoveryBlocked || (!journal && !exporting && (!current || current->length <= 1024 * 1024)) ||
      wifiLease.phase() != companion::WifiHandoffLeasePhase::Empty || wifiPhase != WifiPhase::None) {
    LOG_ERR("COMPANION", "Wi-Fi preparation unavailable");
    return 0;
  }
  clearWifiMaterial();
  wifiMaterial.offer.reader = identity.device;
  wifiMaterial.offer.storageGeneration = identity.storageGeneration;
  wifiMaterial.offer.installation = installation;
  wifiMaterial.offer.transaction = request.transaction;
  wifiMaterial.offer.port = 8080;
  wifiMaterial.offer.lifetimeSeconds = companion::WIFI_HANDOFF_MAX_LIFETIME_SECONDS;
  if (!bluetooth.generateWifiHandoffSecrets(session, wifiMaterial.offer.session, wifiMaterial.offer.key)) {
    clearWifiMaterial();
    return 0;
  }
  wifiMaterial.network.mode = request.mode;
  if (request.mode == companion::WifiNetworkMode::Hotspot) {
    companion::WifiHotspotPassword password;
    char hostname[companion::WIFI_HANDOFF_HOSTNAME_SIZE];
    if (!bluetooth.generateWifiHotspotPassword(session, password) ||
        !companion::wifiHandoffHostname(wifiMaterial.offer.session, hostname)) {
      companion::clearWifiHotspotPassword(password);
      clearWifiMaterial();
      return 0;
    }
    std::copy(password.begin(), password.end(), wifiPassword.begin());
    companion::clearWifiHotspotPassword(password);
    snprintf(wifiSsid.data(), wifiSsid.size(), "%.29s", hostname);
  } else if (!WIFI_STORE.copyLastConnectedCredential(wifiSsid, wifiPassword)) {
    clearWifiMaterial();
    return 0;
  }
  wifiMaterial.network.ssid = wifiSsid.data();
  wifiMaterial.network.password =
      request.mode == companion::WifiNetworkMode::Hotspot ? std::string_view(wifiPassword.data()) : std::string_view{};
  const auto now = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
  const auto result =
      exporting ? wifiLease.prepareExport(wifiMaterial, request, contentReader->exportBinding(), inventory->revision(),
                                          identity.device, identity.storageGeneration, installation, session, now)
      : journal ? wifiLease.prepareJournal(wifiMaterial, request, *journalReceive->receivingDeclaration(),
                                           journalReceive->count(), identity.device, identity.storageGeneration,
                                           installation, session, now)
                : wifiLease.prepare(wifiMaterial, request, *current, identity.device, identity.storageGeneration,
                                    installation, session, now);
  companion::clearWifiHandoffSecrets(wifiMaterial.offer.session, wifiMaterial.offer.key);
  if (result != companion::WifiHandoffLeaseResult::Ok) {
    LOG_ERR("COMPANION", "Wi-Fi lease preparation failed: %u", static_cast<unsigned>(result));
    clearWifiMaterial();
    return 0;
  }
  const auto length = wifiLease.encode(reply, now);
  if (!length) {
    wifiLease.reset();
    clearWifiMaterial();
  }
  return length;
}
size_t CompanionConnectActivity::wifiCommand(const companion::FrameView& request, uint64_t session,
                                             companion::Command& replyCommand, std::span<uint8_t> reply) {
  companion::WifiHandoffPrepare prepare;
  if (companion::decodeWifiHandoffPrepare(request.payload, prepare)) {
    const auto length = prepareWifi(prepare, session, reply);
    if (length) return length;
    replyCommand = companion::Command::Error;
    reply[0] = 5;
    return 1;
  }
  companion::WifiHandoffSessionCommand command;
  if (companion::decodeWifiHandoffSessionCommand(request.payload, command)) {
    const auto result =
        wifiLease.apply(command, session, installation, static_cast<uint64_t>(esp_timer_get_time()) / 1000);
    if (result == companion::WifiHandoffLeaseResult::Ok) {
      if (command.action == companion::WifiHandoffAction::Cancel)
        clearWifiMaterial();
      else
        wifiPhase = WifiPhase::Activating;
      reply[0] = 0;
      return 1;
    }
    LOG_ERR("COMPANION", "Wi-Fi lease command failed: %u", static_cast<unsigned>(result));
  } else
    LOG_ERR("COMPANION", "Invalid Wi-Fi handoff command");
  replyCommand = companion::Command::Error;
  reply[0] = 3;
  return 1;
}
bool CompanionConnectActivity::activateWifi(void* context, const companion::WifiActivationView& view) {
  auto& activity = *static_cast<CompanionConnectActivity*>(context);
  const auto* current = activity.transfer ? activity.transfer->current() : nullptr;
  const bool content =
      current && current->transaction == view.offer.transaction && current->owner == activity.installation &&
      current->storageGeneration == activity.identity.storageGeneration &&
      (current->phase == companion::TransferPhase::Receiving || current->phase == companion::TransferPhase::Verified);
  const bool exporting = activity.contentReader && activity.inventory &&
                         activity.contentReader->exportBinding().boundTo(view.offer.transaction, activity.installation,
                                                                         activity.identity.storageGeneration,
                                                                         activity.inventory->revision());
  if (activity.recoveryBlocked || (!content && !exporting && !activity.journalHandoffMatches(view.offer.transaction)) ||
      !companion::wifiHandoffMatches(view.offer, activity.identity.device, activity.identity.storageGeneration,
                                     activity.installation, view.offer.transaction) ||
      std::string_view(view.ssid) != std::string_view(activity.wifiSsid.data())) {
    LOG_ERR("COMPANION", "Wi-Fi activation transfer changed");
    return false;
  }
  activity.wifiMaterial.offer = view.offer;
  activity.wifiReceivedAt = view.receivedAtMilliseconds;
  activity.bluetooth.stop();
  activity.resetJournalSessions(exporting);
  if (activity.recoveryBlocked) return false;
  activity.ready = false;
  activity.installationSession = 0;
  if (!activity.wifiRadio.begin(view.mode, activity.wifiSsid.data(), activity.wifiPassword.data())) return false;
  activity.wifiPhase = WifiPhase::Connecting;
  logInventoryHeap("Wi-Fi started");
  activity.requestUpdate();
  return true;
}
bool CompanionConnectActivity::stopWifi(bool resumeBluetooth) {
  resetJournalSessions();
  wifiEndpoint.end();
  wifiLease.reset();
  clearWifiMaterial();
  if (!wifiRadio.end()) {
    wifiPhase = WifiPhase::Cleanup;
    wifiCleanupRetryAt = static_cast<uint64_t>(esp_timer_get_time()) / 1000 + 1000;
    ready = false;
    requestUpdate();
    return false;
  }
  wifiPhase = WifiPhase::None;
  if (resumeBluetooth && !recoveryBlocked) {
    ready = bluetooth.begin({workspace.get(), companion::SESSION_WORKSPACE_SIZE});
    installationSession = 0;
    logInventoryHeap("BLE restored");
  }
  requestUpdate();
  return true;
}
void CompanionConnectActivity::pollWifi() {
  if (wifiPhase == WifiPhase::None) return;
  if (wifiPhase == WifiPhase::Cleanup) {
    if (static_cast<uint64_t>(esp_timer_get_time()) / 1000 < wifiCleanupRetryAt) return;
    stopWifi(true);
    return;
  }
  if (wifiPhase == WifiPhase::Activating) {
    const auto result = wifiLease.consume(static_cast<uint64_t>(esp_timer_get_time()) / 1000, {this, activateWifi});
    if (result != companion::WifiHandoffLeaseResult::Ok) {
      LOG_ERR("COMPANION", "Wi-Fi setup failed: %u", static_cast<unsigned>(result));
      stopWifi(true);
      return;
    }
  }
  if (recoveryBlocked || HalMemory::getInternalHeap().freeBytes <= 50 * 1024) {
    LOG_ERR("COMPANION", "Wi-Fi stopped: recovery or heap limit");
    stopWifi(true);
    return;
  }
  if (wifiPhase == WifiPhase::Connecting) {
    const auto now = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
    if (now < wifiReceivedAt ||
        now - wifiReceivedAt >= static_cast<uint64_t>(wifiMaterial.offer.lifetimeSeconds) * 1000) {
      LOG_ERR("COMPANION", "Wi-Fi setup expired");
      stopWifi(true);
      return;
    }
    std::array<uint8_t, 4> address{};
    if (!wifiRadio.address(address)) return;
    const companion::WifiMessageClock clock{nullptr,
                                            [](void*) { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }};
    if (!wifiEndpoint.begin(wifiMaterial.offer, identity.device, identity.storageGeneration, installation,
                            wifiMaterial.offer.transaction, {workspace.get(), companion::SESSION_WORKSPACE_SIZE},
                            wifiReceivedAt, clock, {this, wifiTransferDispatch})) {
      stopWifi(true);
      return;
    }
    if (HalMemory::getInternalHeap().freeBytes <= 50 * 1024) {
      LOG_ERR("COMPANION", "Wi-Fi endpoint exceeded heap limit");
      stopWifi(true);
      return;
    }
    wifiPhase = WifiPhase::Serving;
    clearWifiMaterial();
    requestUpdate();
  }
  if (wifiPhase == WifiPhase::Serving && !wifiEndpoint.poll()) stopWifi(true);
}

void CompanionConnectActivity::onExit() {
  firmwareInstallPending = false;
  wifiEndpoint.end();
  if (!wifiRadio.end()) LOG_ERR("COMPANION", "Wi-Fi teardown incomplete on exit");
  wifiLease.reset();
  clearWifiMaterial();
  bluetooth.stop();
  resetJournalSessions();
  journalExport.reset();
  contentReader.reset();
  inventoryPending = false;
  inventory.reset();
  if (!inventoryStorage.close()) LOG_ERR("COMPANION", "Inventory close failed");
  transferStorage.setDictionaryInstaller(nullptr);
  dictionaryInstaller.reset();
  transfer.reset();
  workspace.reset();
  const auto preferences = companion::restoreReaderPreferenceRuntime(SETTINGS, sdFontSystem);
  if (preferences != companion::ReaderPreferenceApplicationResult::Applied &&
      preferences != companion::ReaderPreferenceApplicationResult::Unchanged)
    LOG_ERR("COMPANION", "Reader preference replay pending on exit: %u", static_cast<unsigned>(preferences));
  ready = false;
  const auto heap = HalMemory::getInternalHeap();
  LOG_INF("COMPANION", "Exit heap: free=%u largest=%u", static_cast<unsigned>(heap.freeBytes),
          static_cast<unsigned>(heap.largestBlockBytes));
  Activity::onExit();
}

void CompanionConnectActivity::loop() {
  if (firmwareInstallPending) {
    const auto now = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
    if (wifiPhase == WifiPhase::Cleanup && now < wifiCleanupRetryAt) return;
    if (now - firmwareInstallAcceptedAt >= 500) performFirmwareInstallation();
    return;
  }
  if (inventoryPending) {
    requestUpdateAndWait();
    const bool prepared = prepareInventory();
    inventoryPending = false;
    if (prepared) {
      const auto readScratch =
          std::span(workspace.get(), companion::SESSION_WORKSPACE_SIZE).subspan(companion::TRANSFER_OFFSET);
      if (companion::admitCompanionHeap(sizeof(companion::HalContentReadNativeOwner),
                                        sizeof(companion::HalContentReadNativeOwner))) {
        contentReader = makeUniqueNoThrow<companion::HalContentReadNativeOwner>(
            readScratch,
            [](void* context) {
              auto& activity = *static_cast<CompanionConnectActivity*>(context);
              const bool bluetoothSession = activity.installationSession &&
                                            activity.installationSession == activity.bluetooth.authenticatedSession();
              const bool exportSession =
                  activity.wifiPhase == WifiPhase::Serving && activity.contentReader && activity.inventory &&
                  activity.contentReader->exportBinding().boundTo(activity.exportTransaction, activity.installation,
                                                                  activity.identity.storageGeneration,
                                                                  activity.inventory->revision());
              return activity.contentReadPermitted() && companion::admitCompanionHeap() &&
                     (bluetoothSession || exportSession);
            },
            this);
        if (!contentReader || !companion::admitCompanionHeap() || !contentReader->prepare() ||
            !contentReader->open(identity.storageGeneration, inventory->revision())) {
          LOG_ERR("COMPANION", "Content export owner unavailable");
          contentReader.reset();
        }
      }
      // Reuse audit/export buffers across pages; they exceed the small task stack.
      journalExport = makeUniqueNoThrow<companion::HalJournalCausalAuditSession>();
      if (!journalExport) LOG_ERR("COMPANION", "OOM: journal export workspace");
      ready = bluetooth.begin({workspace.get(), companion::SESSION_WORKSPACE_SIZE});
      logInventoryHeap(ready ? "radio started" : "radio failed");
    }
    requestUpdate();
    return;
  }
  pollWifi();
  if (wifiPhase == WifiPhase::None) handleCustomInput();
  if (firmwareInstallPending) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && !recoveryBlocked) {
    if (!stopWifi(false)) return;
    onGoHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && ready) {
    forgetConnected();
    requestUpdate();
  }
}
void CompanionConnectActivity::forgetConnected() {
  companion::PairingPeer peer;
  const uint64_t session = bluetooth.authenticatedSession();
  if (!session || !bluetooth.peer(session, peer)) {
    LOG_ERR("COMPANION", "No authenticated connected bond to forget");
    return;
  }
  if (!pairingsAvailable ||
      pairings.forget(peer, {workspace.get() + OUTPUT_START, FRAME_SIZE}) != companion::PairingResult::Ok) {
    pairingsAvailable = false;
    LOG_ERR("COMPANION", "Credential removal failed; bond retained");
    return;
  }
  wifiLease.reset();
  clearWifiMaterial();
  installationSession = 0;
  installation = {};
  resetJournalSessions();
  if (!bluetooth.unpairConnected(session)) LOG_ERR("COMPANION", "Bond removal failed");
}
bool CompanionConnectActivity::handleCustomInput() {
  if (!ready) return false;
  if (HalMemory::getInternalHeap().freeBytes <= 50 * 1024) {
    RenderLock lock;
    wifiLease.reset();
    clearWifiMaterial();
    bluetooth.stop();
    resetJournalSessions();
    ready = false;
    LOG_ERR("COMPANION", "BLE stopped: internal heap fell below 50 KiB");
    requestUpdate();
    return false;
  }
  const auto status = bluetooth.snapshot();
  if (installationSession && installationSession != bluetooth.authenticatedSession()) {
    resetJournalSessions();
    installationSession = 0;
    installation = {};
  }
  if (status != lastStatus) {
    lastStatus = status;
    requestUpdate();
  }
  if (!wifiLease.reconcile(bluetooth.authenticatedSession(), installation,
                           static_cast<uint64_t>(esp_timer_get_time()) / 1000))
    clearWifiMaterial();
  processFrame();
  return false;
}

void CompanionConnectActivity::resetJournalExport() {
  journalExportReady = false;
  if (journalExport && !journalExport->endExport()) LOG_ERR("COMPANION", "Journal export close failed");
}

void CompanionConnectActivity::resetJournalSessions(bool preserveExport) {
  if (!preserveExport) exportTransaction = {};
  if (contentReader && !contentReader->resetSource(preserveExport)) {
    LOG_ERR("COMPANION", "Content export session close failed");
    recoveryBlocked = true;
  }
  if (dictionaryRemovalOwner && !dictionaryRemovalOwner->closeReaders()) {
    LOG_ERR("COMPANION", "Dictionary removal readers could not close");
    recoveryBlocked = true;
  }
  dictionaryRemovalOwner.reset();
  removalOwner.reset();
  removalActive = false;
  resetJournalExport();
  if (journalReceive && !journalReceive->close()) {
    LOG_ERR("COMPANION", "Journal receive close failed");
    recoveryBlocked = true;
  }
  journalReceive.reset();
  if (legacyBackup && !legacyBackup->close()) {
    LOG_ERR("COMPANION", "Legacy backup close failed");
    recoveryBlocked = true;
  }
  legacyBackup.reset();
}

size_t CompanionConnectActivity::legacyBackupReply(std::span<const uint8_t> request, std::span<uint8_t> reply) {
#if LILA_TINTA
  companion::LegacyBackupRequest parsed;
  if (recoveryBlocked || journalReceive || journalExportReady ||
      !companion::decodeLegacyBackupRequest(request, parsed) || parsed.generation != identity.storageGeneration)
    return 0;
  const auto* content = transfer ? transfer->current() : nullptr;
  if (content && content->phase != companion::TransferPhase::Committed &&
      content->phase != companion::TransferPhase::Aborted)
    return 0;
  if (parsed.operation == companion::LegacyBackupOperation::Capture) {
    companion::Identity course{};
    bool present = false;
    auto context = makeUniqueNoThrow<companion::HalTintaJournalMergeCommitContext>();
    if (!context) {
      LOG_ERR("COMPANION", "OOM: backup course binding");
      return 0;
    }
    if (!parsed.bound || !context->installedCourse(course, present) || !present || course != parsed.course) return 0;
  }
  if (!legacyBackup) {
    resetJournalExport();
    journalExport.reset();
    // Retained paths, handles and scratch exceed the task stack; reuse until close/disconnect.
    legacyBackup = makeUniqueNoThrow<companion::HalLegacyBackupExchange>();
    if (!legacyBackup) {
      LOG_ERR("COMPANION", "OOM: legacy backup exchange");
      return 0;
    }
  }
  const auto length = legacyBackup->dispatch(parsed, identity.device, reply);
  if (length >= companion::LEGACY_BACKUP_REPLY_HEADER_SIZE &&
      parsed.operation == companion::LegacyBackupOperation::Close &&
      reply[4] == static_cast<uint8_t>(companion::LegacyBackupResult::Ok))
    legacyBackup.reset();
  return length;
#else
  return 0;
#endif
}

size_t CompanionConnectActivity::firmwareInfoReply(std::span<const uint8_t> request, std::span<uint8_t> reply) {
  if (recoveryBlocked || request.size() != companion::FIRMWARE_INFO_REQUEST_SIZE ||
      reply.size() < companion::FIRMWARE_READER_INFO_SIZE || request[0] != 'F' || request[1] != 'W' ||
      request[2] != 'Q' || request[3] != 1 ||
      !std::equal(identity.storageGeneration.begin(), identity.storageGeneration.end(), request.begin() + 4))
    return 0;
  companion::FirmwareReaderInfo info;
  if (!freshFirmwareInfo(info) ||
      !companion::encodeFirmwareReaderInfo(info, reply.first(companion::FIRMWARE_READER_INFO_SIZE)))
    return 0;
  return companion::FIRMWARE_READER_INFO_SIZE;
}
bool CompanionConnectActivity::freshFirmwareInfo(companion::FirmwareReaderInfo& info) {
  info = {};
  info.board = board();
  info.battery = static_cast<uint8_t>(std::min<uint16_t>(powerManager.getBatteryPercentage(), 100));
  info.generation = identity.storageGeneration;
  info.chip = firmware_flash::runningPartitionChipId();
  info.partitionBytes = firmware_flash::nextPartitionBytes();
  if (!workspace ||
      !firmware_flash::runningImageDigest(
          std::span(workspace.get(), companion::SESSION_WORKSPACE_SIZE).subspan(companion::TRANSFER_OFFSET),
          info.runningBuild))
    return 0;
  // Existing lookup buffers exceed 256 bytes; this read-only query frees them before returning.
  auto formats = makeUniqueNoThrow<companion::HalJournalFormatInventory>();
  uint8_t versions = 0;
  if (!formats || !formats->inspect(versions)) {
    LOG_ERR("COMPANION", "Firmware journal information unavailable");
    return 0;
  }
  info.journalVersions = versions;
  if (versions) info.stateSchema = 1;
  for (const char* path :
       {companion::COURSE_BINDING_PATH, companion::COURSE_BINDING_STAGE, companion::COURSE_BINDING_BACKUP,
        companion::COURSE_STATE_MIGRATION, companion::COURSE_STATE_MIGRATION_STAGE,
        companion::COURSE_STATE_MIGRATION_DONE, companion::COURSE_STATE_MIGRATION_DONE_STAGE,
        companion::COURSE_MARK_MIGRATION_PATHS.intent, companion::COURSE_MARK_MIGRATION_PATHS.stage,
        companion::COURSE_MARK_MIGRATION_PATHS.done, companion::COURSE_MARK_MIGRATION_PATHS.doneStage}) {
    uint64_t size = 0;
    const auto status = transferStorage.stat(path, size);
    if (status == companion::FileStatus::Error) return 0;
    if (status == companion::FileStatus::Present) info.stateSchema = 1;
  }
  return companion::validFirmwareReaderInfo(info);
}
size_t CompanionConnectActivity::firmwareInstallReply(std::span<const uint8_t> request, std::span<uint8_t> reply) {
  if (reply.size() < 17 || recoveryBlocked || firmwareInstallPending || wifiPhase != WifiPhase::None ||
      journalReceive || legacyBackup || !transfer || !workspace ||
      !companion::decodeFirmwareInstallRequest(request, firmwareAuthorization.request))
    return 0;
  if (!closeContentReaders()) return 0;
  firmwareAuthorization.owner = installation;
  const auto* current = transfer->current();
  const auto* manifest = transfer->contentManifest();
  companion::FirmwareReaderInfo fresh;
  if (!current || !manifest || !freshFirmwareInfo(fresh)) return 0;
  const auto result = companion::admitFirmwareInstallation(
      transferStorage, firmwareAuthorization, fresh, *current, *manifest, transfer->destination(),
      std::span(workspace.get(), companion::SESSION_WORKSPACE_SIZE).subspan(companion::TRANSFER_OFFSET));
  reply[0] = static_cast<uint8_t>(result);
  std::copy(firmwareAuthorization.request.transaction.begin(), firmwareAuthorization.request.transaction.end(),
            reply.begin() + 1);
  if (result == companion::FirmwareInstallIntentResult::Ok) {
    firmwareInstallPending = true;
    firmwareInstallAcceptedAt = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
  } else {
    LOG_ERR("COMPANION", "Firmware installation admission failed: %u", static_cast<unsigned>(result));
  }
  return 17;
}
void CompanionConnectActivity::performFirmwareInstallation() {
  companion::FirmwareReaderInfo fresh;
  const auto* current = transfer ? transfer->current() : nullptr;
  const auto* manifest = transfer ? transfer->contentManifest() : nullptr;
  if (!workspace || !current || !manifest || !freshFirmwareInfo(fresh) ||
      companion::admitFirmwareInstallation(
          transferStorage, firmwareAuthorization, fresh, *current, *manifest, transfer->destination(),
          std::span(workspace.get(), companion::SESSION_WORKSPACE_SIZE).subspan(companion::TRANSFER_OFFSET)) !=
          companion::FirmwareInstallIntentResult::Ok) {
    LOG_ERR("COMPANION", "Firmware installation recheck failed; intent retained");
    firmwareInstallPending = false;
    requestUpdate();
    return;
  }
  if (!stopWifi(false)) return;
  bluetooth.stop();
  ready = false;
  resetJournalSessions();
  journalExport.reset();
  inventory.reset();
  if (!inventoryStorage.close()) {
    LOG_ERR("COMPANION", "Firmware installation inventory close failed");
    recoveryBlocked = true;
  }
  if (!recoveryBlocked && powerManager.getBatteryPercentage() >= firmwareAuthorization.request.minimumBattery &&
      transferStorage.verify(companion::FIRMWARE_STAGE_DESTINATION, firmwareAuthorization.request.length,
                             firmwareAuthorization.request.hash,
                             {workspace.get(), companion::SESSION_WORKSPACE_SIZE})) {
    requestUpdateAndWait();
    const auto result = firmware_flash::flashFromSdPath(companion::FIRMWARE_STAGE_DESTINATION, nullptr, nullptr, false,
                                                        firmwareAuthorization.request.hash);
    if (result == firmware_flash::Result::OK) {
      LOG_INF("COMPANION", "Firmware flashed; restarting for boot verification");
      ESP.restart();
      return;
    }
    LOG_ERR("COMPANION", "Firmware flash failed: %s", firmware_flash::resultName(result));
  } else {
    LOG_ERR("COMPANION", "Firmware flash blocked after teardown");
  }
  firmwareInstallPending = false;
  recoveryBlocked = true;
  requestUpdate();
}

size_t CompanionConnectActivity::journalExchangeReply(std::span<const uint8_t> request,
                                                      const companion::Identity& owner, std::span<uint8_t> reply) {
  if (!closeContentReaders()) return 0;
  if (request.size() == companion::COURSE_SWITCH_REQUEST_SIZE) {
#if LILA_TINTA
    if (recoveryBlocked || journalReceive || legacyBackup || !transfer || !workspace) return 0;
    resetJournalExport();
    return companion::handleCourseSwitch(transferStorage, *transfer, identity.storageGeneration, owner, request, reply,
                                         {workspace.get(), companion::SESSION_WORKSPACE_SIZE});
#else
    return 0;
#endif
  }
  if (request.size() == companion::LEGACY_BACKUP_REQUEST_SIZE) return legacyBackupReply(request, reply);
  if (legacyBackup) return 0;
  if (request.size() == companion::JOURNAL_EXPORT_REQUEST_SIZE)
    return journalReceive ? 0 : journalExportReply(request, reply);
#if LILA_TINTA
  if (request.size() == companion::JOURNAL_MERGE_READINESS_REQUEST_SIZE) {
    if (recoveryBlocked || journalReceive) return 0;
    const auto* content = transfer ? transfer->current() : nullptr;
    if (content && content->phase != companion::TransferPhase::Committed &&
        content->phase != companion::TransferPhase::Aborted)
      return 0;
    resetJournalExport();
    // Context owns pack handles, baseline bytes and scratch beyond the stack budget.
    auto context = makeUniqueNoThrow<companion::HalTintaJournalMergeCommitContext>();
    if (!context) {
      LOG_ERR("COMPANION", "OOM: journal merge readiness context");
      return 0;
    }
    return context->readinessReply(request, identity.storageGeneration, reply);
  }
#endif
  if (request.size() == companion::JOURNAL_STATE_REQUEST_SIZE) {
    if (recoveryBlocked || journalReceive) return 0;
    resetJournalExport();
    return companion::handleJournalStateQuery(identity.storageGeneration, request, reply);
  }

#if LILA_TINTA
  if (request.size() == companion::TINTA_MIGRATION_ADMISSION_SIZE && request[0] == 'T' && request[1] == 'M' &&
      request[2] == 'A') {
    if (recoveryBlocked || journalReceive || reply.size() < 24) return 0;
    const auto* content = transfer ? transfer->current() : nullptr;
    if (content && content->phase != companion::TransferPhase::Committed &&
        content->phase != companion::TransferPhase::Aborted)
      return 0;
    resetJournalExport();
    auto context = makeUniqueNoThrow<companion::HalTintaJournalMergeCommitContext>();
    if (!context) {
      LOG_ERR("COMPANION", "OOM: Tinta migration admission context");
      return 0;
    }
    return context->migrationAdmissionReply(request, identity, owner, reply);
  }
#endif
  companion::JournalMergeRequestView parsed;
  if (recoveryBlocked || reply.size() < 24 || !companion::decodeJournalMergeRequest(request, parsed)) return 0;
  const auto* content = transfer ? transfer->current() : nullptr;
  if (content && content->phase != companion::TransferPhase::Committed &&
      content->phase != companion::TransferPhase::Aborted)
    return 0;
  resetJournalExport();
  if (!journalReceive) {
    // Candidate scratch and decoded envelopes exceed the stack budget; reuse for this connection.
    journalReceive = makeUniqueNoThrow<companion::HalJournalMergeReceiveSession>();
    if (!journalReceive) {
      LOG_ERR("COMPANION", "OOM: journal receive session");
      return 0;
    }
  }
  auto result = journalReceive->dispatch(request, owner, identity.storageGeneration, nullptr, nullptr);
#if LILA_TINTA
  companion::Identity course{};
  bool reconcile = false;
  if (parsed.operation == companion::JournalMergeOperation::Commit &&
      result == companion::TintaJournalResult::Invalid) {
    // Installed pack, receipt and hashing workspaces exceed the task stack budget.
    auto context = makeUniqueNoThrow<companion::HalTintaJournalMergeCommitContext>();
    if (!context) {
      LOG_ERR("COMPANION", "OOM: Tinta merge commit context");
      result = companion::TintaJournalResult::IoError;
    } else if (context->prepare(parsed.declaration, identity.storageGeneration) ||
               context->prepareMigration(parsed.declaration, identity, owner)) {
      course = *context->course();
      result =
          journalReceive->dispatch(request, owner, identity.storageGeneration, context->course(), context->catalog());
      reconcile = result == companion::TintaJournalResult::Ok;
    }
  }
  if (parsed.operation == companion::JournalMergeOperation::Commit &&
      (result == companion::TintaJournalResult::Ok || result == companion::TintaJournalResult::Duplicate) &&
      !reconcile) {
    auto context = makeUniqueNoThrow<companion::HalTintaJournalMergeCommitContext>();
    if (!context || !context->installedCourse(course, reconcile)) {
      LOG_ERR("COMPANION", "Cannot resolve merged learner state");
      recoveryBlocked = true;
      result = companion::TintaJournalResult::IoError;
    }
  }
#endif
  std::fill_n(reply.begin(), 24, uint8_t{0});
  reply[0] = 1;
  reply[1] = static_cast<uint8_t>(result);
  std::copy(parsed.transaction.begin(), parsed.transaction.end(), reply.begin() + 4);
  companion::tinta_body_detail::write(reply, 20, journalReceive->count(), 4);
  if (!journalReceive->hasBinding() ||
      ((result == companion::TintaJournalResult::Ok || result == companion::TintaJournalResult::Duplicate) &&
       parsed.operation == companion::JournalMergeOperation::Commit))
    resetJournalSessions();
#if LILA_TINTA
  if (reconcile) {
    // Release candidate/pack workspaces before allocating replay and publication workspaces.
    auto reconciliation = makeUniqueNoThrow<companion::HalTintaMergedJournalReconciliation>(course, identityStorage);
    if (!reconciliation || !reconciliation->run()) {
      LOG_ERR("COMPANION", "Tinta merge requires derived-state recovery");
      recoveryBlocked = true;
      reply[1] = static_cast<uint8_t>(companion::TintaJournalResult::IoError);
    }
  }
#endif
  if (parsed.operation == companion::JournalMergeOperation::Commit &&
      (reply[1] == static_cast<uint8_t>(companion::TintaJournalResult::Ok) ||
       reply[1] == static_cast<uint8_t>(companion::TintaJournalResult::Duplicate))) {
    const auto preferences = companion::restoreReaderPreferenceRuntime(SETTINGS, sdFontSystem, false);
    if (preferences != companion::ReaderPreferenceApplicationResult::Applied &&
        preferences != companion::ReaderPreferenceApplicationResult::Unchanged) {
      LOG_ERR("COMPANION", "Merged reader preferences require recovery: %u", static_cast<unsigned>(preferences));
      reply[1] = static_cast<uint8_t>(companion::TintaJournalResult::Unavailable);
    }
  }
  return 24;
}

size_t CompanionConnectActivity::journalExportReply(std::span<const uint8_t> request, std::span<uint8_t> reply) {
  if (!journalExport || recoveryBlocked || request.size() != companion::JOURNAL_EXPORT_REQUEST_SIZE) return 0;
  if (!journalExportReady) {
    if (!std::all_of(request.begin(), request.end(), [](uint8_t byte) { return byte == 0; })) return 0;
    if (!journalExport->beginExport()) return 0;
    journalExportReady = true;
  }
  const size_t length = journalExport->exportPage(request, reply);
  if (!length) resetJournalExport();
  return length;
}

size_t CompanionConnectActivity::dispatchTransfer(const companion::FrameView& request, const companion::Identity& owner,
                                                  std::span<uint8_t> response) {
  if (response.empty()) {
    LOG_ERR("COMPANION", "Missing transfer reply buffer");
    return 0;
  }
  if (!transfer || recoveryBlocked) {
    LOG_ERR("COMPANION", "Transfer recovery unavailable");
    response[0] = static_cast<uint8_t>(companion::TransferResult::IoError);
    return 1;
  }
  if (Storage.exists(companion::FIRMWARE_INSTALL_INTENT_PATH) ||
      Storage.exists(companion::FIRMWARE_INSTALL_INTENT_STAGE)) {
    LOG_ERR("COMPANION", "Content transfer blocked by firmware installation intent");
    response[0] = static_cast<uint8_t>(companion::TransferResult::Busy);
    return 1;
  }
  if (journalReceive || legacyBackup) {
    LOG_ERR("COMPANION", "Content transfer conflicts with journal receive session");
    response[0] = static_cast<uint8_t>(companion::TransferResult::Busy);
    return 1;
  }
  if (!closeContentReaders()) {
    response[0] = static_cast<uint8_t>(companion::TransferResult::IoError);
    return 1;
  }
  const auto outcome = companion::dispatchTransfer(*transfer, identity.storageGeneration, request.command,
                                                   request.payload, owner, response);
  if (outcome.inventoryChanged) sdFontSystem.markRegistryDirty();
  if (outcome.inventoryChanged && inventory) inventory->invalidate();
  recoveryBlocked |= outcome.recoveryBlocked;
  return outcome.length;
}
companion::WifiDispatchReply CompanionConnectActivity::wifiTransferDispatch(void* context,
                                                                            const companion::FrameView& request,
                                                                            const companion::Identity& owner,
                                                                            std::span<uint8_t> response) {
  auto& activity = *static_cast<CompanionConnectActivity*>(context);
  if (owner != activity.installation) {
    LOG_ERR("COMPANION", "Wi-Fi installation mismatch");
    if (response.empty()) return {companion::Command::Error, 0};
    response[0] = 2;
    return {companion::Command::Error, 1};
  }
  if (request.command == companion::Command::ReadContent) {
    const auto length =
        activity.contentReader && activity.inventory && activity.contentReadPermitted() &&
                activity.contentReader->open(activity.identity.storageGeneration, activity.inventory->revision())
            ? activity.contentReader->wifiReply(activity.exportTransaction, owner, activity.identity.storageGeneration,
                                                activity.inventory->revision(), request.payload, response)
            : 0;
    if (length) return {request.command, length};
    if (response.empty()) return {companion::Command::Error, 0};
    response[0] = 1;
    return {companion::Command::Error, 1};
  }
  if (request.command == companion::Command::RemoveContent)
    return {request.command, activity.removalReply(true, owner, request.payload, response)};
  if (request.command == companion::Command::JournalFormats) return {request.command, journalFormatReply(response)};
  if (request.command == companion::Command::ExchangeChanges) {
    const size_t length = request.payload.size() >= owner.size()
                              ? activity.journalExchangeReply(request.payload.subspan(owner.size()), owner, response)
                              : 0;
    if (length) return {request.command, length};
    if (response.empty()) return {companion::Command::Error, 0};
    response[0] = 1;
    return {companion::Command::Error, 1};
  }
  return {request.command, activity.dispatchTransfer(request, owner, response)};
}

void CompanionConnectActivity::processFrame() {
  auto all = std::span(workspace.get(), companion::SESSION_WORKSPACE_SIZE);
  auto input = all.subspan(OUTPUT_START, FRAME_SIZE);
  uint64_t session = 0;
  const size_t count = bluetooth.receive(input, session);
  if (!count) return;
  companion::FrameView request;
  if (companion::decodeFrame(input.first(count), true, request) != companion::FrameError::None || request.response)
    return;
  if (installationSession != session) {
    resetJournalSessions();
    installationSession = 0;
    installation = {};
  }
  auto response = all.subspan(OUTPUT_START + FRAME_SIZE, FRAME_SIZE);
  auto payload = response.subspan(companion::FRAME_HEADER_SIZE);
  companion::Command command = request.command;
  size_t length = 0;
  if (request.command == companion::Command::Discover && request.payload.empty()) {
    companion::DeviceDescriptor descriptor;
    descriptor.device = identity.device;
    descriptor.storageGeneration = identity.storageGeneration;
    descriptor.board = board();
    firmware_flash::runningImageDigest(all.subspan(companion::TRANSFER_OFFSET), descriptor.runningBuild);
    descriptor.batteryPercent = static_cast<uint8_t>(std::min<uint16_t>(powerManager.getBatteryPercentage(), 100));
    descriptor.capabilities = companion::CAPABILITY_DECLARED_TRANSFERS | companion::CAPABILITY_JOURNAL_FORMATS;
    descriptor.capabilities |= companion::CAPABILITY_FONT_TRANSFERS;
    descriptor.capabilities |= companion::CAPABILITY_EPUB_REMOVALS;
    descriptor.capabilities |= companion::CAPABILITY_FONT_REMOVALS;
    descriptor.capabilities |= companion::CAPABILITY_DICTIONARY_REMOVALS;
    if (contentReader)
      descriptor.capabilities |= companion::CAPABILITY_CONTENT_READS | companion::CAPABILITY_CONTENT_METADATA |
                                 companion::CAPABILITY_WIFI_CONTENT_READS;
    if (dictionaryInstaller) descriptor.capabilities |= companion::CAPABILITY_DICTIONARY_TRANSFERS;
#if CROSSPOINT_VECTOR_FONTS
    descriptor.capabilities |= companion::CAPABILITY_VECTOR_FONT_TRANSFERS;
#endif
    if (journalExport) descriptor.capabilities |= companion::CAPABILITY_JOURNAL_EXPORT;
#if LILA_TINTA
    descriptor.capabilities |= companion::CAPABILITY_COURSE_TRANSFERS | companion::CAPABILITY_COURSE_SWITCHES;
    if (journalExport) descriptor.capabilities |= companion::CAPABILITY_JOURNAL_MERGE_READINESS;
#endif
    length = companion::encodeRecord(descriptor, payload);
  } else if ((request.command == companion::Command::RegisterInstallation ||
              request.command == companion::Command::AuthenticateInstallation) &&
             request.payload.size() == 48) {
    resetJournalSessions();
    companion::Identity candidate;
    companion::PairingSecret secret;
    companion::PairingPeer peer;
    std::copy_n(request.payload.begin(), candidate.size(), candidate.begin());
    std::copy_n(request.payload.begin() + candidate.size(), secret.size(), secret.begin());
    bool authorized = false;
    if (pairingsAvailable && bluetooth.peer(session, peer)) {
      if (request.command == companion::Command::RegisterInstallation) {
        const auto result = pairings.add(candidate, secret, peer, payload);
        if (result == companion::PairingResult::IoError) pairingsAvailable = false;
        authorized = result == companion::PairingResult::Ok;
      } else
        authorized = pairings.authenticate(candidate, secret) && pairings.boundTo(candidate, peer);
    }
    std::fill(secret.begin(), secret.end(), 0);
    if (authorized) {
      installation = candidate;
      installationSession = session;
    } else {
      installation = {};
      installationSession = 0;
      command = companion::Command::Error;
    }
    std::fill(payload.begin(), payload.end(), 0);
    payload[0] = authorized ? 0 : 2;
    length = 1;
  } else if (request.command == companion::Command::Inventory) {
    companion::PairingPeer peer;
    const bool authorized = installationSession == session && bluetooth.peer(session, peer) && pairingsAvailable &&
                            pairings.boundTo(installation, peer);
    if (inventory) {
      length = inventoryReply(*inventory, authorized, request.payload, payload);
    } else {
      payload[0] = static_cast<uint8_t>(authorized ? companion::InventoryResult::IoError
                                                   : companion::InventoryResult::Unauthorized);
      length = 1;
    }
  } else if ((request.command == companion::Command::ReadContent ||
              request.command == companion::Command::ContentMetadata ||
              request.command == companion::Command::PrepareContentHandoff)) {
    companion::PairingPeer peer;
    const bool authorized = installationSession == session && bluetooth.peer(session, peer) && pairingsAvailable &&
                            pairings.boundTo(installation, peer);
    length = request.command == companion::Command::ReadContent ? contentReadReply(authorized, request.payload, payload)
             : request.command == companion::Command::ContentMetadata
                 ? contentMetadataReply(authorized, request.payload, payload)
                 : contentHandoffReply(authorized, request.payload, payload);
    if (!length) {
      command = companion::Command::Error;
      payload[0] = 1;
      length = 1;
    }
  } else if (request.command == companion::Command::RemoveContent) {
    companion::PairingPeer peer;
    const bool authorized = installationSession == session && bluetooth.peer(session, peer) && pairingsAvailable &&
                            pairings.boundTo(installation, peer);
    length = removalReply(authorized, installation, request.payload, payload);
  } else if (request.command == companion::Command::ExchangeChanges) {
    companion::PairingPeer peer;
    const bool authorized = installationSession == session && bluetooth.peer(session, peer) && pairingsAvailable &&
                            pairings.boundTo(installation, peer);
    if (authorized) length = journalExchangeReply(request.payload, installation, payload);
    if (!length) {
      command = companion::Command::Error;
      payload[0] = authorized ? 1 : 2;
      length = 1;
    }
  } else if (request.command == companion::Command::JournalFormats) {
    companion::PairingPeer peer;
    const bool authorized = installationSession == session && bluetooth.peer(session, peer) && pairingsAvailable &&
                            pairings.boundTo(installation, peer);
    payload[0] = authorized ? 1 : 2;
    length = 1;
    if (authorized && request.payload.empty()) length = journalFormatReply(payload);
  } else if (request.command == companion::Command::InstallFirmware) {
    companion::PairingPeer peer;
    const bool authorized = installationSession == session && bluetooth.peer(session, peer) && pairingsAvailable &&
                            pairings.boundTo(installation, peer);
    if (authorized) {
      length = request.payload.size() == companion::FIRMWARE_INSTALL_REQUEST_SIZE
                   ? firmwareInstallReply(request.payload, payload)
                   : firmwareInfoReply(request.payload, payload);
    }
    if (!length) {
      command = companion::Command::Error;
      payload[0] = authorized ? 1 : 2;
      length = 1;
    }
  } else if (request.command == companion::Command::WifiHandoff) {
    companion::PairingPeer peer;
    if (installationSession != session || !bluetooth.peer(session, peer) || !pairingsAvailable ||
        !pairings.boundTo(installation, peer)) {
      command = companion::Command::Error;
      payload[0] = 2;
      length = 1;
    } else {
      length = wifiCommand(request, session, command, payload);
    }
  } else if (request.command >= companion::Command::BeginTransfer && request.command <= companion::Command::Abort) {
    companion::PairingPeer peer;
    if (installationSession != session || !bluetooth.peer(session, peer) || !pairingsAvailable ||
        !pairings.boundTo(installation, peer)) {
      command = companion::Command::Error;
      payload[0] = 2;
      length = 1;
    } else {
      length = dispatchTransfer(request, installation, payload);
    }
  } else {
    command = companion::Command::Error;
    payload[0] = 1;  // unsupported command or body
    length = 1;
  }
  // encodeFrame requires disjoint payload and destination. Move the small record
  // into the already-consumed input region before assembling the response.
  std::fill(input.begin(), input.end(), 0);
  std::copy_n(payload.begin(), length, input.begin());
  companion::FrameView reply{command, true, request.requestId, input.first(length)};
  const size_t encoded = companion::encodeFrame(reply, response);
  const bool delivered = encoded && bluetooth.send(response.first(encoded), session);
  if (!delivered) {
    LOG_ERR("COMPANION", "BLE response interrupted");
    if (command == companion::Command::WifiHandoff && wifiLease.phase() == companion::WifiHandoffLeasePhase::Prepared) {
      wifiLease.reset();
      clearWifiMaterial();
    }
  }
  volatile uint8_t* consumed = input.data();
  volatile uint8_t* sent = response.data();
  for (size_t at = 0; at < input.size(); ++at) consumed[at] = 0;
  for (size_t at = 0; at < response.size(); ++at) sent[at] = 0;
  if (recoveryBlocked) {
    RenderLock lock;
    wifiLease.reset();
    clearWifiMaterial();
    bluetooth.stop();
    ready = false;
    requestUpdate();
  }
}

void CompanionConnectActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto status = bluetooth.snapshot();
  const char* message = firmwareInstallPending               ? tr(STR_UPDATING)
                        : inventoryPending                   ? tr(STR_COMPANION_BUILDING_INVENTORY)
                        : recoveryBlocked                    ? tr(STR_COMPANION_RECOVERY_FAILED)
                        : wifiPhase == WifiPhase::Connecting ? tr(STR_COMPANION_WIFI_CONNECTING)
                        : wifiPhase == WifiPhase::Serving    ? tr(STR_COMPANION_WIFI_TRANSFERRING)
                        : wifiPhase == WifiPhase::Cleanup    ? tr(STR_COMPANION_WIFI_CLEANUP)
                        : !ready                             ? tr(STR_COMPANION_START_FAILED)
                        : status.state == HalCompanionBluetooth::State::Authenticated ? tr(STR_COMPANION_PAIRED)
                        : status.state == HalCompanionBluetooth::State::Connected     ? tr(STR_COMPANION_PAIRING)
                                                                                      : tr(STR_COMPANION_WAITING);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_COMPANION_CONNECT_SYNC), message);
  GUI.drawButtonHints(renderer, recoveryBlocked || inventoryPending ? "" : tr(STR_BACK),
                      ready ? tr(STR_COMPANION_FORGET_CONNECTED) : "", "", "");
  if (status.showPasskey) {
    snprintf(passkeyText, sizeof(passkeyText), "%06lu", static_cast<unsigned long>(status.passkey));
    GUI.drawSubHeader(
        renderer, Rect{0, metrics.topPadding + metrics.headerHeight, renderer.getScreenWidth(), metrics.headerHeight},
        tr(STR_COMPANION_PAIRING_CODE));
    GUI.drawPopup(renderer, passkeyText);
  } else {
    GUI.drawPopup(renderer, message);
  }
}

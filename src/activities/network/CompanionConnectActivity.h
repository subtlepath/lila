#pragma once

#include <CompanionFirmwareInstallAdmission.h>
#include <CompanionFrame.h>
#include <CompanionIdentity.h>
#include <CompanionInventoryIndex.h>
#include <CompanionTransfer.h>
#include <CompanionWifiHandoffLease.h>
#include <HalCompanionBluetooth.h>
#include <HalCompanionWifiRadio.h>
#include <HalCompanionWifiSession.h>
#include <HalContentReadNativeOwner.h>
#include <HalEpubRemovalNativeOwner.h>
#include <HalIdentityStorage.h>
#include <HalInventoryIndexStorage.h>
#include <HalJournalCausalAuditSession.h>
#include <HalJournalMergeReceiveSession.h>
#include <HalLegacyBackupExchange.h>
#include <HalPairingsStorage.h>
#include <HalTransferStorage.h>

#include <memory>
#include <optional>

#include "CompanionFontRemovalSettings.h"
#include "activities/Activity.h"

namespace companion {
struct WifiDispatchReply;
}

class CompanionConnectActivity final : public Activity {
 public:
  CompanionConnectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool recovering = false)
      : Activity("CompanionConnect", renderer, mappedInput), recoveryBlocked(recovering) {}
  void onEnter() override;
  void onExit() override;
  bool preventAutoSleep() override {
    return firmwareInstallPending || ready || recoveryBlocked || inventoryPending || wifiPhase != WifiPhase::None;
  }
  bool requiresExclusiveStorageLoop() const override {
    return firmwareInstallPending || recoveryBlocked || inventoryPending || removalActive;
  }
  bool handleHomeGesture() override {
    return firmwareInstallPending || recoveryBlocked || inventoryPending || wifiRadio.ownsResources();
  }

 private:
  void loop() override;
  void render(RenderLock&&) override;
  bool handleCustomInput();
  void processFrame();
  size_t dispatchTransfer(const companion::FrameView& request, const companion::Identity& owner,
                          std::span<uint8_t> response);
  static companion::WifiDispatchReply wifiTransferDispatch(void* context, const companion::FrameView& request,
                                                           const companion::Identity& owner,
                                                           std::span<uint8_t> response);
  size_t wifiCommand(const companion::FrameView& request, uint64_t session, companion::Command& replyCommand,
                     std::span<uint8_t> reply);
  size_t prepareWifi(const companion::WifiHandoffPrepare& request, uint64_t session, std::span<uint8_t> reply);
  bool journalHandoffMatches(const companion::Identity& transaction) const;
  void pollWifi();
  bool stopWifi(bool resumeBluetooth);
  void clearWifiMaterial();
  static bool activateWifi(void* context, const companion::WifiActivationView& view);
  void forgetConnected();
  bool prepareInventory();
  bool refreshAfterRemoval();
  bool removalPermitted() const;
  bool contentReadPermitted() const;
  size_t removalReply(bool authorized, const companion::Identity& owner, std::span<const uint8_t> request,
                      std::span<uint8_t> reply);
  size_t contentMetadataReply(bool authorized, std::span<const uint8_t> request, std::span<uint8_t> reply);
  size_t contentReadReply(bool authorized, std::span<const uint8_t> request, std::span<uint8_t> reply);
  size_t contentHandoffReply(bool authorized, std::span<const uint8_t> request, std::span<uint8_t> reply);
  bool closeContentReaders();
  bool freshFirmwareInfo(companion::FirmwareReaderInfo& output);
  size_t firmwareInstallReply(std::span<const uint8_t> request, std::span<uint8_t> reply);
  void performFirmwareInstallation();
  size_t firmwareInfoReply(std::span<const uint8_t> request, std::span<uint8_t> reply);
  size_t journalExportReply(std::span<const uint8_t> request, std::span<uint8_t> reply);
  void resetJournalExport();
  void resetJournalSessions(bool preserveExport = false);
  size_t journalExchangeReply(std::span<const uint8_t> request, const companion::Identity& owner,
                              std::span<uint8_t> reply);
  size_t legacyBackupReply(std::span<const uint8_t> request, std::span<uint8_t> reply);

  std::unique_ptr<uint8_t[]> workspace;
  CompanionFontRemovalSettings fontRemovalSettings;
  std::unique_ptr<companion::HalEpubRemovalNativeOwner> removalOwner;
  std::unique_ptr<companion::HalContentReadNativeOwner> contentReader;
  bool removalActive = false;
  companion::HalIdentityStorage identityStorage;
  companion::IdentityState identity;
  companion::HalTransferStorage transferStorage;
  std::optional<companion::Transfer> transfer;
  std::unique_ptr<companion::HalDictionaryTransferInstaller> dictionaryInstaller;
  companion::HalInventoryIndexStorage inventoryStorage;
  std::optional<companion::IndexedInventoryCatalog> inventory;
  bool inventoryPending = false;
  std::unique_ptr<companion::HalJournalCausalAuditSession> journalExport;
  bool journalExportReady = false;
  std::unique_ptr<companion::HalJournalMergeReceiveSession> journalReceive;
  std::unique_ptr<companion::HalLegacyBackupExchange> legacyBackup;
  companion::HalPairingsStorage pairingsStorage;
  companion::Pairings pairings{pairingsStorage};
  companion::Identity installation{};
  companion::Identity exportTransaction{};
  uint64_t installationSession = 0;
  bool pairingsAvailable = false;
  HalCompanionBluetooth bluetooth;
  HalCompanionBluetooth::Snapshot lastStatus;
  companion::FirmwareInstallAuthorization firmwareAuthorization;
  bool firmwareInstallPending = false;
  uint64_t firmwareInstallAcceptedAt = 0;
  bool ready = false;
  bool recoveryBlocked = false;
  char passkeyText[8]{};
  enum class WifiPhase { None, Activating, Connecting, Serving, Cleanup };
  WifiPhase wifiPhase = WifiPhase::None;
  companion::WifiHandoffLease wifiLease;
  companion::HalCompanionWifiRadio wifiRadio;
  companion::HalCompanionWifiSession wifiEndpoint;
  companion::WifiNetworkOffer wifiMaterial;
  std::array<char, 33> wifiSsid{};
  std::array<char, 65> wifiPassword{};
  uint64_t wifiReceivedAt = 0, wifiCleanupRetryAt = 0;
};

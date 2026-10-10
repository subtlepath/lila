#pragma once

#include "CompanionWifiHandoff.h"
#include "CompanionWifiRequest.h"
#include "CompanionWorkspace.h"
#include "HalCompanionWifiCipher.h"

namespace companion {
struct WifiMessageClock {
  void* context = nullptr;
  uint64_t (*milliseconds)(void*) = nullptr;
};
struct WifiDispatchReply {
  Command command = Command::Error;
  size_t length = 0;
};
struct WifiMessageDispatch {
  void* context = nullptr;
  WifiDispatchReply (*execute)(void*, const FrameView&, const Identity&, std::span<uint8_t>) = nullptr;
};
enum class WifiMessageResult {
  Ok,
  Inactive,
  Busy,
  Expired,
  InvalidMessage,
  InvalidRequest,
  DispatchError,
  CryptoError
};
struct WifiMessageReply {
  WifiMessageResult result;
  std::span<const uint8_t> bytes;
};

// Session-owned, serialized by the activity. BLE must release this workspace first.
class HalCompanionWifiMessages final {
 public:
  static constexpr size_t MAX_MESSAGE_SIZE =
      HalCompanionWifiCipher::HEADER_SIZE + HalCompanionWifiCipher::MAX_PAYLOAD + HalCompanionWifiCipher::TAG_SIZE;
  HalCompanionWifiMessages() = default;
  ~HalCompanionWifiMessages();
  HalCompanionWifiMessages(const HalCompanionWifiMessages&) = delete;
  HalCompanionWifiMessages& operator=(const HalCompanionWifiMessages&) = delete;
  bool begin(const WifiHandoffOffer& offer, const Identity& reader, const Identity& generation,
             const Identity& installation, const Identity& transaction, std::span<uint8_t> workspace,
             uint64_t receivedAtMilliseconds, WifiMessageClock clock);
  void end();
  bool pollDeadline();
  bool finishing() const { return finishRequested; }
  // Serialized dispatch only. Copy request fields out before taking this loan.
  // end() revokes permission immediately but preserves bytes until release.
  bool acquireWorkspace(const Identity& session);
  bool workspaceOwned(const Identity& session) const;
  bool releaseWorkspace(const Identity& session);
  // Fill at most MAX_MESSAGE_SIZE bytes; process() consumes exactly length.
  std::span<uint8_t> requestBuffer();
  // Reply borrows workspace until the next request or end(). A failure ends the session except Busy.
  WifiMessageReply process(size_t length, WifiMessageDispatch dispatch);

 private:
  static constexpr size_t PLAIN_OFFSET = MAX_MESSAGE_SIZE;
  static constexpr size_t PAYLOAD_OFFSET = PLAIN_OFFSET + HalCompanionWifiCipher::MAX_PAYLOAD;
  static constexpr size_t REPLY_OFFSET = PAYLOAD_OFFSET + MAX_CONTROL_PAYLOAD;
  static constexpr size_t WIRE_OFFSET = REPLY_OFFSET + HalCompanionWifiCipher::MAX_PAYLOAD;
  static_assert(WIRE_OFFSET + MAX_MESSAGE_SIZE <= TRANSFER_OFFSET);
  bool validAt(uint64_t now) const;
  WifiMessageReply fail(WifiMessageResult result);
  HalCompanionWifiCipher cipher;
  std::span<uint8_t> workspace;
  Identity owner{}, transaction{}, generation{}, session{};
  WifiMessageClock clock;
  uint64_t lastActivity = 0, lifetimeMilliseconds = 0;
  bool active = false, activated = false, processing = false;
  bool finishRequested = false;
  bool dispatching = false, workspaceLeased = false, cleanupPending = false;
};
}  // namespace companion

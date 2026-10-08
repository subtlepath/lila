#pragma once

#include "CompanionWifiHttpRequest.h"
#include "HalCompanionWifiMessages.h"

namespace companion {
// Activity-owned; callers stop this listener before destroying its message session.
class HalCompanionWifiHttp final {
 public:
  explicit HalCompanionWifiHttp(HalCompanionWifiMessages& messages) : messages(messages) {}
  ~HalCompanionWifiHttp();
  HalCompanionWifiHttp(const HalCompanionWifiHttp&) = delete;
  HalCompanionWifiHttp& operator=(const HalCompanionWifiHttp&) = delete;
  bool begin(uint16_t port, WifiMessageClock clock, WifiMessageDispatch dispatch);
  // False means the listener/session ended; the radio owner restores BLE.
  bool poll();
  void end();

 private:
  static constexpr uint64_t CONNECTION_TIMEOUT_MS = 5000;
  static constexpr size_t IO_BUDGET = 2048;
  void closeConnection();
  void closeSockets();
  bool receive();
  bool sendReply();
  bool socketFailure(const char* operation);
  HalCompanionWifiMessages& messages;
  WifiHttpRequest parser;
  WifiMessageClock clock;
  WifiMessageDispatch dispatch;
  int listener = -1, connection = -1;
  uint64_t phaseStarted = 0;
  std::span<const uint8_t> reply;
  std::array<char, 128> header{};
  size_t headerLength = 0, headerSent = 0, bodySent = 0;
  bool sending = false;
};
}  // namespace companion

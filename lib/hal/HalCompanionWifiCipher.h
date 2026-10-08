#pragma once

#include <mbedtls/gcm.h>

#include <array>
#include <span>

#include "CompanionRecords.h"

namespace companion {
enum class WifiMessageDirection : uint8_t { AppleToReader = 0, ReaderToApple = 1 };
enum class WifiCipherResult { Ok, InvalidSession, InvalidMessage, Exhausted, Authentication, CryptoError };

// Session-owned and serialized by its caller. Every handoff requires a fresh key.
class HalCompanionWifiCipher final {
 public:
  static constexpr size_t MAX_PAYLOAD = 1036;
  static constexpr size_t HEADER_SIZE = 31;
  static constexpr size_t TAG_SIZE = 16;
  HalCompanionWifiCipher();
  ~HalCompanionWifiCipher();
  HalCompanionWifiCipher(const HalCompanionWifiCipher&) = delete;
  HalCompanionWifiCipher& operator=(const HalCompanionWifiCipher&) = delete;
  bool begin(std::span<const uint8_t> key, const Identity& session, WifiMessageDirection sending);
  void end();
  WifiCipherResult seal(std::span<const uint8_t> plaintext, std::span<uint8_t> output, size_t& length);
  WifiCipherResult open(std::span<const uint8_t> message, std::span<uint8_t> output, size_t& length);

 private:
  void envelope(WifiMessageDirection direction, uint64_t counter, size_t length);
  mbedtls_gcm_context context;
  Identity session{};
  std::array<uint8_t, HEADER_SIZE> header{};
  std::array<uint8_t, 12> nonce{};
  uint64_t sendCounter = 1, receiveCounter = 1;
  WifiMessageDirection sending = WifiMessageDirection::ReaderToApple;
  bool active = false, sendExhausted = false, receiveExhausted = false;
};
}  // namespace companion

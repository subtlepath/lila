#include "HalCompanionWifiCipher.h"

#include <Logging.h>

#include <algorithm>

namespace companion {
namespace {
bool overlaps(std::span<const uint8_t> input, std::span<uint8_t> output) {
  const auto source = reinterpret_cast<uintptr_t>(input.data());
  const auto destination = reinterpret_cast<uintptr_t>(output.data());
  return !input.empty() && !output.empty() &&
         (source <= destination ? destination - source < input.size() : source - destination < output.size());
}
WifiCipherResult failure(WifiCipherResult result, const char* reason) {
  LOG_ERR("COMPANION", "Wi-Fi cipher: %s", reason);
  return result;
}
}  // namespace
HalCompanionWifiCipher::HalCompanionWifiCipher() { mbedtls_gcm_init(&context); }
HalCompanionWifiCipher::~HalCompanionWifiCipher() { mbedtls_gcm_free(&context); }
void HalCompanionWifiCipher::end() {
  active = false;
  mbedtls_gcm_free(&context);
  mbedtls_gcm_init(&context);
  session.fill(0);
  header.fill(0);
  nonce.fill(0);
}
bool HalCompanionWifiCipher::begin(std::span<const uint8_t> key, const Identity& identifier,
                                   WifiMessageDirection direction) {
  end();
  if (key.size() != 32 || !std::any_of(identifier.begin(), identifier.end(), [](uint8_t byte) { return byte != 0; }) ||
      static_cast<uint8_t>(direction) > 1) {
    failure(WifiCipherResult::InvalidSession, "invalid key, session or direction");
    return false;
  }
  if (mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key.data(), 256) != 0) {
    end();
    failure(WifiCipherResult::CryptoError, "key setup failed");
    return false;
  }
  session = identifier;
  sending = direction;
  sendCounter = receiveCounter = 1;
  sendExhausted = receiveExhausted = false;
  active = true;
  return true;
}
void HalCompanionWifiCipher::envelope(WifiMessageDirection direction, uint64_t counter, size_t length) {
  header[0] = nonce[0] = 'L';
  header[1] = nonce[1] = 'W';
  header[2] = nonce[2] = 'H';
  header[3] = 1;
  std::copy(session.begin(), session.end(), header.begin() + 4);
  header[20] = nonce[3] = static_cast<uint8_t>(direction);
  for (unsigned at = 0; at < 8; ++at) header[21 + at] = nonce[4 + at] = static_cast<uint8_t>(counter >> (at * 8));
  header[29] = static_cast<uint8_t>(length);
  header[30] = static_cast<uint8_t>(length >> 8);
}
WifiCipherResult HalCompanionWifiCipher::seal(std::span<const uint8_t> plaintext, std::span<uint8_t> output,
                                              size_t& length) {
  length = 0;
  if (!active) return failure(WifiCipherResult::InvalidSession, "inactive handoff");
  if (sendExhausted) return failure(WifiCipherResult::Exhausted, "send counter exhausted");
  if (plaintext.empty() || plaintext.size() > MAX_PAYLOAD ||
      output.size() < HEADER_SIZE + plaintext.size() + TAG_SIZE || overlaps(plaintext, output))
    return failure(WifiCipherResult::InvalidMessage, "invalid encryption buffers");
  const auto counter = sendCounter;
  if (counter == UINT64_MAX)
    sendExhausted = true;
  else
    ++sendCounter;
  envelope(sending, counter, plaintext.size());
  std::copy(header.begin(), header.end(), output.begin());
  if (mbedtls_gcm_crypt_and_tag(&context, MBEDTLS_GCM_ENCRYPT, plaintext.size(), nonce.data(), nonce.size(),
                                header.data(), header.size(), plaintext.data(), output.data() + HEADER_SIZE, TAG_SIZE,
                                output.data() + HEADER_SIZE + plaintext.size()) != 0) {
    std::fill_n(output.begin(), HEADER_SIZE + plaintext.size() + TAG_SIZE, 0);
    end();
    return failure(WifiCipherResult::CryptoError, "encryption failed");
  }
  length = HEADER_SIZE + plaintext.size() + TAG_SIZE;
  return WifiCipherResult::Ok;
}
WifiCipherResult HalCompanionWifiCipher::open(std::span<const uint8_t> message, std::span<uint8_t> output,
                                              size_t& length) {
  length = 0;
  if (!active) return failure(WifiCipherResult::InvalidSession, "inactive handoff");
  if (receiveExhausted) return failure(WifiCipherResult::Exhausted, "receive counter exhausted");
  if (message.size() <= HEADER_SIZE + TAG_SIZE || message.size() > HEADER_SIZE + MAX_PAYLOAD + TAG_SIZE)
    return failure(WifiCipherResult::InvalidMessage, "invalid encrypted length");
  const auto payloadLength = message.size() - HEADER_SIZE - TAG_SIZE;
  if (output.size() < payloadLength || overlaps(message, output))
    return failure(WifiCipherResult::InvalidMessage, "invalid decryption buffers");
  const auto direction = sending == WifiMessageDirection::AppleToReader ? WifiMessageDirection::ReaderToApple
                                                                        : WifiMessageDirection::AppleToReader;
  envelope(direction, receiveCounter, payloadLength);
  if (!std::equal(header.begin(), header.end(), message.begin()))
    return failure(WifiCipherResult::InvalidMessage, "unexpected session, direction or counter");
  if (mbedtls_gcm_auth_decrypt(&context, payloadLength, nonce.data(), nonce.size(), header.data(), header.size(),
                               message.data() + HEADER_SIZE + payloadLength, TAG_SIZE, message.data() + HEADER_SIZE,
                               output.data()) != 0) {
    std::fill_n(output.begin(), payloadLength, 0);
    return failure(WifiCipherResult::Authentication, "message authentication failed");
  }
  if (receiveCounter == UINT64_MAX)
    receiveExhausted = true;
  else
    ++receiveCounter;
  length = payloadLength;
  return WifiCipherResult::Ok;
}
}  // namespace companion

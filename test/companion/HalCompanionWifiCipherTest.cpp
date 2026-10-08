#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <numeric>

#include "HalCompanionWifiCipher.h"
using namespace companion;

TEST(HalCompanionWifiCipherTest, SharedFixtureTamperingReplayReflectionAndEnd) {
  std::array<uint8_t, 32> key;
  std::iota(key.begin(), key.end(), 0);
  Identity session;
  session.fill(7);
  HalCompanionWifiCipher apple, reader;
  ASSERT_TRUE(apple.begin(key, session, WifiMessageDirection::AppleToReader));
  ASSERT_TRUE(reader.begin(key, session, WifiMessageDirection::ReaderToApple));
  static constexpr char TEXT[] = "control frame";
  const auto payload = std::span(reinterpret_cast<const uint8_t*>(TEXT), sizeof(TEXT) - 1);
  std::array<uint8_t, 1083> wire{}, output{};
  size_t length = 0, opened = 0;
  ASSERT_EQ(apple.seal(payload, wire, length), WifiCipherResult::Ok);
  std::ifstream fixture(WIFI_FIXTURE);
  ASSERT_TRUE(fixture);
  const std::string json(std::istreambuf_iterator<char>(fixture), {});
  auto start = json.find("\"binaryHex\"");
  ASSERT_NE(start, std::string::npos);
  start = json.find('"', json.find(':', start)) + 1;
  const auto expected = json.substr(start, json.find('"', start) - start);
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(length * 2);
  for (auto byte : std::span(wire).first(length)) {
    hex.push_back(HEX_DIGITS[byte >> 4]);
    hex.push_back(HEX_DIGITS[byte & 15]);
  }
  ASSERT_EQ(hex, expected);
  for (size_t at = 0; at < length; ++at) {
    wire[at] ^= 1;
    output.fill(0xa5);
    EXPECT_EQ(reader.open(std::span(wire).first(length), output, opened),
              at < 31 ? WifiCipherResult::InvalidMessage : WifiCipherResult::Authentication);
    EXPECT_EQ(opened, 0u);
    if (at >= 31)
      EXPECT_TRUE(std::all_of(output.begin(), output.begin() + payload.size(), [](uint8_t byte) { return byte == 0; }));
    wire[at] ^= 1;
  }
  ASSERT_EQ(reader.open(std::span(wire).first(length), output, opened), WifiCipherResult::Ok);
  EXPECT_EQ(opened, payload.size());
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(), output.begin()));
  EXPECT_EQ(reader.open(std::span(wire).first(length), output, opened), WifiCipherResult::InvalidMessage);
  EXPECT_EQ(apple.open(std::span(wire).first(length), output, opened), WifiCipherResult::InvalidMessage);
  ASSERT_EQ(reader.seal(payload, wire, length), WifiCipherResult::Ok);
  ASSERT_EQ(apple.open(std::span(wire).first(length), output, opened), WifiCipherResult::Ok);
  reader.end();
  EXPECT_EQ(reader.seal(payload, wire, length), WifiCipherResult::InvalidSession);
}

TEST(HalCompanionWifiCipherTest, RejectsInvalidSessionsBoundsAndAliasingWithoutConsumingCounter) {
  HalCompanionWifiCipher cipher;
  Identity session{};
  std::array<uint8_t, 32> key{};
  EXPECT_FALSE(cipher.begin(key, session, WifiMessageDirection::ReaderToApple));
  session.fill(1);
  EXPECT_FALSE(cipher.begin(std::span(key).first(31), session, WifiMessageDirection::ReaderToApple));
  ASSERT_TRUE(cipher.begin(key, session, WifiMessageDirection::ReaderToApple));
  std::array<uint8_t, 1083> wire{};
  size_t length = 99;
  EXPECT_EQ(cipher.seal({}, wire, length), WifiCipherResult::InvalidMessage);
  EXPECT_EQ(length, 0u);
  EXPECT_EQ(cipher.seal(std::span(wire).first(1), wire, length), WifiCipherResult::InvalidMessage);
  std::array<uint8_t, 1037> oversized{};
  EXPECT_EQ(cipher.seal(oversized, wire, length), WifiCipherResult::InvalidMessage);
  ASSERT_EQ(cipher.seal(std::span(oversized).first(1036), wire, length), WifiCipherResult::Ok);
  EXPECT_EQ(length, wire.size());
  EXPECT_EQ(wire[21], 1u);
  HalCompanionWifiCipher peer;
  ASSERT_TRUE(peer.begin(key, session, WifiMessageDirection::AppleToReader));
  EXPECT_EQ(peer.open(wire, wire, length), WifiCipherResult::InvalidMessage);
  ASSERT_EQ(peer.open(wire, oversized, length), WifiCipherResult::Ok);
  EXPECT_EQ(length, 1036u);
}

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lib/Companion/CompanionWifiNetworkOffer.h"
using namespace companion;
namespace {
std::vector<uint8_t> fixture(const char* name) {
  std::ifstream file(std::string(WIFI_NETWORK_FIXTURES) + name);
  EXPECT_TRUE(file);
  const std::string json(std::istreambuf_iterator<char>(file), {});
  const auto key = json.find("\"binaryHex\"");
  if (key == std::string::npos) return {};
  const auto start = json.find('"', json.find(':', key)) + 1;
  const auto end = json.find('"', start);
  std::vector<uint8_t> bytes;
  bytes.reserve((end - start) / 2);
  for (size_t at = start; at < end; at += 2)
    bytes.push_back(static_cast<uint8_t>(std::stoul(json.substr(at, 2), nullptr, 16)));
  return bytes;
}
}  // namespace
TEST(CompanionWifiNetworkOfferTest, SharedSavedAndHotspotFixturesRoundTripAndBorrowStrings) {
  for (const bool hotspot : {false, true}) {
    auto bytes = fixture(hotspot ? "/WifiNetworkOfferHotspot.json" : "/WifiNetworkOfferSaved.json");
    WifiNetworkOffer value;
    ASSERT_TRUE(decodeWifiNetworkOffer(bytes, value));
    EXPECT_EQ(value.network.mode, hotspot ? WifiNetworkMode::Hotspot : WifiNetworkMode::SavedNetwork);
    EXPECT_EQ(value.network.ssid, hotspot ? "lila-test" : "Reader Home");
    EXPECT_EQ(value.network.password, hotspot ? "fixture-password" : "");
    EXPECT_EQ(value.network.ssid.data(), reinterpret_cast<const char*>(bytes.data() + WIFI_NETWORK_OFFER_HEADER_SIZE));
    std::array<uint8_t, WIFI_NETWORK_OFFER_MAX_SIZE> encoded{};
    const auto count = encodeWifiNetworkOffer(value, encoded);
    ASSERT_EQ(count, bytes.size());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), encoded.begin()));
    const auto previous = value;
    for (size_t length = 0; length < bytes.size(); ++length) {
      EXPECT_FALSE(decodeWifiNetworkOffer(std::span(bytes).first(length), value));
      EXPECT_EQ(value, previous);
    }
    for (const size_t at : {size_t{0}, size_t{1}, size_t{122}, size_t{123}, size_t{124}}) {
      auto invalid = bytes;
      invalid[at] = 255;
      EXPECT_FALSE(decodeWifiNetworkOffer(invalid, value));
      EXPECT_EQ(value, previous);
    }
    auto invalid = bytes;
    invalid[125] = 0;
    EXPECT_FALSE(decodeWifiNetworkOffer(invalid, value));
    EXPECT_EQ(value, previous);
    invalid = bytes;
    invalid.push_back(0);
    EXPECT_FALSE(decodeWifiNetworkOffer(invalid, value));
    encoded.fill(7);
    EXPECT_EQ(encodeWifiNetworkOffer(previous, std::span(encoded).first(count - 1)), 0u);
    EXPECT_TRUE(std::all_of(encoded.begin(), encoded.end(), [](uint8_t byte) { return byte == 7; }));
  }
}
TEST(CompanionWifiNetworkOfferTest, BoundsAndSavedPasswordPrivacyAreEnforced) {
  const std::string maxSsid(32, 's'), maxPassword(63, 'p'), longSsid(33, 's'), longPassword(64, 'p');
  EXPECT_TRUE(validWifiNetworkDescription({WifiNetworkMode::Hotspot, maxSsid, maxPassword}));
  EXPECT_TRUE(validWifiNetworkDescription({WifiNetworkMode::SavedNetwork, maxSsid, {}}));
  EXPECT_FALSE(validWifiNetworkDescription({WifiNetworkMode::SavedNetwork, "ssid", "private-network-password"}));
  EXPECT_FALSE(validWifiNetworkDescription({WifiNetworkMode::SavedNetwork, {}, {}}));
  EXPECT_FALSE(validWifiNetworkDescription({WifiNetworkMode::Hotspot, longSsid, maxPassword}));
  EXPECT_FALSE(validWifiNetworkDescription({WifiNetworkMode::Hotspot, "ssid", longPassword}));
  EXPECT_FALSE(validWifiNetworkDescription({WifiNetworkMode::Hotspot, "ssid", "short"}));
  EXPECT_FALSE(validWifiNetworkDescription({WifiNetworkMode::Hotspot, "line\nfeed", maxPassword}));
  EXPECT_FALSE(validWifiNetworkDescription({WifiNetworkMode::Hotspot, "ssid", "bad\npassword"}));
  static constexpr char RAW_SSID[] = {'n', static_cast<char>(0xff)};
  EXPECT_TRUE(validWifiNetworkDescription({WifiNetworkMode::SavedNetwork, {RAW_SSID, 2}, {}}));
  EXPECT_FALSE(validWifiNetworkDescription({WifiNetworkMode::Hotspot, {RAW_SSID, 2}, maxPassword}));
  auto bytes = fixture("/WifiNetworkOfferHotspot.json");
  WifiNetworkOffer value;
  ASSERT_TRUE(decodeWifiNetworkOffer(bytes, value));
  value.network = {WifiNetworkMode::Hotspot, maxSsid, maxPassword};
  std::array<uint8_t, WIFI_NETWORK_OFFER_MAX_SIZE> encoded{};
  ASSERT_EQ(encodeWifiNetworkOffer(value, encoded), encoded.size());
  WifiNetworkOffer decoded;
  ASSERT_TRUE(decodeWifiNetworkOffer(encoded, decoded));
  EXPECT_EQ(decoded, value);
  encoded[122] = static_cast<uint8_t>(WifiNetworkMode::SavedNetwork);
  EXPECT_FALSE(decodeWifiNetworkOffer(encoded, decoded));
}

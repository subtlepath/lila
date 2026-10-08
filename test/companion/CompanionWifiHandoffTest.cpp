#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <numeric>

// Arduino Print.h defines the hexadecimal print base.
#define HEX 16
#include "lib/Companion/CompanionWifiHandoff.h"
#undef HEX
using namespace companion;
TEST(CompanionWifiHandoffTest, SharedFixtureRoundTripAndIdentityBinding) {
  WifiHandoffOffer offer;
  offer.reader.fill(1);
  offer.storageGeneration.fill(2);
  offer.installation.fill(3);
  offer.transaction.fill(4);
  offer.session.fill(5);
  std::iota(offer.key.begin(), offer.key.end(), 0);
  offer.address = {192, 168, 4, 1};
  offer.port = 8080;
  offer.lifetimeSeconds = 30;
  std::array<uint8_t, WIFI_HANDOFF_OFFER_SIZE> bytes{};
  ASSERT_EQ(encodeWifiHandoffOffer(offer, bytes), bytes.size());
  std::ifstream file(WIFI_HANDOFF_FIXTURE);
  ASSERT_TRUE(file);
  const std::string json(std::istreambuf_iterator<char>(file), {});
  auto start = json.find("\"binaryHex\"");
  ASSERT_NE(start, std::string::npos);
  start = json.find('"', json.find(':', start)) + 1;
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    encoded.push_back(HEX_DIGITS[byte >> 4]);
    encoded.push_back(HEX_DIGITS[byte & 15]);
  }
  EXPECT_EQ(encoded, json.substr(start, json.find('"', start) - start));
  WifiHandoffOffer decoded;
  ASSERT_TRUE(decodeWifiHandoffOffer(bytes, decoded));
  EXPECT_EQ(decoded, offer);
  EXPECT_TRUE(wifiHandoffMatches(offer, offer.reader, offer.storageGeneration, offer.installation, offer.transaction));
  Identity different{};
  different.fill(9);
  EXPECT_FALSE(wifiHandoffMatches(offer, different, offer.storageGeneration, offer.installation, offer.transaction));
  EXPECT_FALSE(wifiHandoffMatches(offer, offer.reader, different, offer.installation, offer.transaction));
  EXPECT_FALSE(wifiHandoffMatches(offer, offer.reader, offer.storageGeneration, different, offer.transaction));
  EXPECT_FALSE(wifiHandoffMatches(offer, offer.reader, offer.storageGeneration, offer.installation, different));
  for (size_t offset : {size_t{1}, size_t{17}, size_t{33}, size_t{49}, size_t{65}, size_t{81}}) {
    auto invalid = bytes;
    std::fill_n(invalid.begin() + offset, offset == 81 ? 32 : 16, 0);
    EXPECT_FALSE(decodeWifiHandoffOffer(invalid, decoded));
    EXPECT_EQ(decoded, offer);
  }
  for (size_t length = 0; length < bytes.size(); ++length)
    EXPECT_FALSE(decodeWifiHandoffOffer(std::span(bytes).first(length), decoded));
  auto invalid = bytes;
  invalid[0] = 2;
  EXPECT_FALSE(decodeWifiHandoffOffer(invalid, decoded));
  invalid = bytes;
  invalid[119] = 121;
  EXPECT_FALSE(decodeWifiHandoffOffer(invalid, decoded));
  auto discovered = offer;
  discovered.address.fill(0);
  ASSERT_EQ(encodeWifiHandoffOffer(discovered, bytes), bytes.size());
  ASSERT_TRUE(decodeWifiHandoffOffer(bytes, decoded));
  EXPECT_EQ(decoded, discovered);
  for (const uint8_t first : {0, 127, 224}) {
    discovered.address = {first, 1, 2, 3};
    EXPECT_FALSE(validWifiHandoffOffer(discovered));
  }
}

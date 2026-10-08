#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>

#include "lib/Companion/CompanionWifiHandoffCommands.h"
using namespace companion;
namespace {
std::string fixtureHex(const char* name) {
  std::ifstream file(WIFI_HANDOFF_COMMAND_FIXTURE);
  const std::string text(std::istreambuf_iterator<char>(file), {});
  const auto key = text.find(std::string("\"") + name + "\"");
  if (key == std::string::npos) return {};
  const auto start = text.find('"', text.find(':', key)) + 1;
  return text.substr(start, text.find('"', start) - start);
}
std::string hex(std::span<const uint8_t> bytes) {
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string value;
  value.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    value.push_back(DIGITS[byte >> 4]);
    value.push_back(DIGITS[byte & 15]);
  }
  return value;
}
}  // namespace
TEST(CompanionWifiHandoffCommandsTest, SharedPrepareFixturesAndStrictValidation) {
  WifiHandoffPrepare value;
  value.transaction.fill(4);
  std::array<uint8_t, WIFI_HANDOFF_PREPARE_SIZE> bytes{};
  for (const auto mode : {WifiNetworkMode::SavedNetwork, WifiNetworkMode::Hotspot}) {
    value.mode = mode;
    ASSERT_EQ(encodeWifiHandoffPrepare(value, bytes), bytes.size());
    EXPECT_EQ(hex(bytes), fixtureHex(mode == WifiNetworkMode::Hotspot ? "prepareHotspot" : "prepareSaved"));
    WifiHandoffPrepare parsed;
    ASSERT_TRUE(decodeWifiHandoffPrepare(bytes, parsed));
    EXPECT_EQ(parsed, value);
    for (size_t length = 0; length < bytes.size(); ++length) {
      EXPECT_FALSE(decodeWifiHandoffPrepare(std::span(bytes).first(length), parsed));
      EXPECT_EQ(parsed, value);
    }
    for (const size_t at : {size_t{0}, size_t{1}, size_t{2}}) {
      auto invalid = bytes;
      invalid[at] = 255;
      EXPECT_FALSE(decodeWifiHandoffPrepare(invalid, parsed));
      EXPECT_EQ(parsed, value);
    }
    auto invalid = bytes;
    std::fill(invalid.begin() + 3, invalid.end(), 0);
    EXPECT_FALSE(decodeWifiHandoffPrepare(invalid, parsed));
    EXPECT_EQ(parsed, value);
    bytes.fill(7);
    EXPECT_EQ(encodeWifiHandoffPrepare(value, std::span(bytes).first(bytes.size() - 1)), 0u);
    EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 7; }));
  }
  value.transaction.fill(0);
  EXPECT_EQ(encodeWifiHandoffPrepare(value, bytes), 0u);
}
TEST(CompanionWifiHandoffCommandsTest, SharedSessionFixturesRequireBothIdentitiesAndExactAction) {
  WifiHandoffSessionCommand value;
  value.transaction.fill(4);
  value.session.fill(5);
  std::array<uint8_t, WIFI_HANDOFF_SESSION_COMMAND_SIZE> bytes{};
  for (const auto action : {WifiHandoffAction::Activate, WifiHandoffAction::Cancel}) {
    value.action = action;
    ASSERT_EQ(encodeWifiHandoffSessionCommand(value, bytes), bytes.size());
    EXPECT_EQ(hex(bytes), fixtureHex(action == WifiHandoffAction::Activate ? "activate" : "cancel"));
    WifiHandoffSessionCommand parsed;
    ASSERT_TRUE(decodeWifiHandoffSessionCommand(bytes, parsed));
    EXPECT_EQ(parsed, value);
    for (size_t length = 0; length < bytes.size(); ++length) {
      EXPECT_FALSE(decodeWifiHandoffSessionCommand(std::span(bytes).first(length), parsed));
      EXPECT_EQ(parsed, value);
    }
    for (const size_t offset : {size_t{2}, size_t{18}}) {
      auto invalid = bytes;
      std::fill_n(invalid.begin() + offset, 16, 0);
      EXPECT_FALSE(decodeWifiHandoffSessionCommand(invalid, parsed));
      EXPECT_EQ(parsed, value);
    }
    for (const uint8_t invalidAction : {0, 1, 4, 255}) {
      auto invalid = bytes;
      invalid[1] = invalidAction;
      EXPECT_FALSE(decodeWifiHandoffSessionCommand(invalid, parsed));
      EXPECT_EQ(parsed, value);
    }
    auto invalid = bytes;
    invalid[0] = 2;
    EXPECT_FALSE(decodeWifiHandoffSessionCommand(invalid, parsed));
    std::array<uint8_t, WIFI_HANDOFF_SESSION_COMMAND_SIZE + 1> oversized{};
    std::copy(bytes.begin(), bytes.end(), oversized.begin());
    EXPECT_FALSE(decodeWifiHandoffSessionCommand(oversized, parsed));
  }
  value.action = WifiHandoffAction::Prepare;
  EXPECT_EQ(encodeWifiHandoffSessionCommand(value, bytes), 0u);
  value.action = WifiHandoffAction::Activate;
  value.session.fill(0);
  EXPECT_EQ(encodeWifiHandoffSessionCommand(value, bytes), 0u);
}

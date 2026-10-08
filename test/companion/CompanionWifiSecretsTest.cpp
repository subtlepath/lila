#include <gtest/gtest.h>

#include "lib/Companion/CompanionWifiSecrets.h"
using namespace companion;
namespace {
struct RandomFixture {
  unsigned calls = 0;
  unsigned mode = 0;
  static bool fill(void* context, std::span<uint8_t> bytes) {
    auto& self = *static_cast<RandomFixture*>(context);
    EXPECT_EQ(bytes.size(), 48u);
    ++self.calls;
    for (size_t at = 0; at < bytes.size(); ++at) bytes[at] = static_cast<uint8_t>(at + self.calls);
    if (self.mode == 1) return false;
    if (self.mode == 2) std::fill(bytes.begin(), bytes.begin() + 16, 0);
    if (self.mode == 3) std::fill(bytes.begin() + 16, bytes.end(), 0);
    return true;
  }
};
}  // namespace
TEST(CompanionWifiSecretsTest, FreshSourceUsedForEveryGenerationAndExplicitClear) {
  Identity session{};
  Digest key{};
  RandomFixture source;
  ASSERT_TRUE(generateWifiHandoffSecrets(session, key, {&source, RandomFixture::fill}));
  EXPECT_EQ(session.front(), 1);
  EXPECT_EQ(session.back(), 16);
  EXPECT_EQ(key.front(), 17);
  EXPECT_EQ(key.back(), 48);
  const auto previousSession = session;
  const auto previousKey = key;
  ASSERT_TRUE(generateWifiHandoffSecrets(session, key, {&source, RandomFixture::fill}));
  EXPECT_EQ(source.calls, 2u);
  EXPECT_NE(session, previousSession);
  EXPECT_NE(key, previousKey);
  clearWifiHandoffSecrets(session, key);
  EXPECT_EQ(session, Identity{});
  EXPECT_EQ(key, Digest{});
}
TEST(CompanionWifiSecretsTest, MissingFailingOrZeroSourceClearsPreviousOutputs) {
  for (unsigned mode = 0; mode <= 3; ++mode) {
    Identity session;
    session.fill(9);
    Digest key;
    key.fill(10);
    RandomFixture source;
    source.mode = mode;
    const auto random = mode == 0 ? WifiRandomSource{} : WifiRandomSource{&source, RandomFixture::fill};
    EXPECT_FALSE(generateWifiHandoffSecrets(session, key, random));
    EXPECT_EQ(session, Identity{});
    EXPECT_EQ(key, Digest{});
    EXPECT_EQ(source.calls, mode == 0 ? 0u : 1u);
  }
}

TEST(CompanionWifiSecretsTest, HotspotPasswordUsesSeparateDrawAndTerminatedHex) {
  struct Source {
    unsigned calls = 0;
    static bool fill(void* context, std::span<uint8_t> bytes) {
      auto& self = *static_cast<Source*>(context);
      ++self.calls;
      EXPECT_EQ(bytes.size(), self.calls == 1 ? 48u : 16u);
      std::fill(bytes.begin(), bytes.end(), self.calls == 1 ? 0x11 : 0xab);
      return true;
    }
  } source;
  Identity session{};
  Digest key{};
  WifiHotspotPassword password{};
  ASSERT_TRUE(generateWifiHandoffSecrets(session, key, {&source, Source::fill}));
  ASSERT_TRUE(generateWifiHotspotPassword(password, {&source, Source::fill}));
  EXPECT_EQ(source.calls, 2u);
  EXPECT_STREQ(password.data(), "abababababababababababababababab");
  EXPECT_EQ(key.front(), 0x11);
  EXPECT_EQ(password.back(), '\0');
  clearWifiHotspotPassword(password);
  EXPECT_EQ(password, WifiHotspotPassword{});
}
TEST(CompanionWifiSecretsTest, HotspotFailureWipesPreviousPassword) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    WifiHotspotPassword password;
    password.fill('x');
    WifiRandomSource source{&mode, [](void* context, std::span<uint8_t> bytes) {
                              EXPECT_EQ(bytes.size(), 16u);
                              const auto mode = *static_cast<unsigned*>(context);
                              std::fill(bytes.begin(), bytes.end(), mode == 1 ? 0xff : 0);
                              return mode != 1;
                            }};
    if (mode == 0) source.fill = nullptr;
    EXPECT_FALSE(generateWifiHotspotPassword(password, source));
    EXPECT_EQ(password, WifiHotspotPassword{});
  }
}

#include <gtest/gtest.h>

#include <array>
#include <string>

#include "lib/Serialization/WifiCredentialSnapshot.h"
TEST(WifiCredentialSnapshotTest, FullLengthAndSubstringSourcesAreTerminated) {
  std::array<char, 33> ssid;
  std::array<char, 65> password;
  const std::string name(34, 's'), key(66, 'a');
  ASSERT_TRUE(copyWifiCredentialSnapshot(std::string_view(name).substr(1, 32), std::string_view(key).substr(1, 64),
                                         ssid, password));
  EXPECT_EQ(ssid.back(), '\0');
  EXPECT_EQ(password.back(), '\0');
  EXPECT_STREQ(ssid.data(), std::string(32, 's').c_str());
  EXPECT_STREQ(password.data(), std::string(64, 'a').c_str());
  ASSERT_TRUE(copyWifiCredentialSnapshot("open", "", ssid, password));
  EXPECT_STREQ(ssid.data(), "open");
  EXPECT_EQ(password, (std::array<char, 65>{}));
}
TEST(WifiCredentialSnapshotTest, InvalidOrMissingSourcesClearPreviousSecrets) {
  for (unsigned mode = 0; mode < 5; ++mode) {
    std::array<char, 33> ssid;
    std::array<char, 65> password;
    ssid.fill('s');
    password.fill('p');
    const auto name = mode == 0   ? std::string{}
                      : mode == 1 ? std::string(33, 's')
                      : mode == 2 ? std::string("a\0b", 3)
                                  : std::string("valid");
    const auto key = mode == 3 ? std::string(65, 'p') : mode == 4 ? std::string("a\0b", 3) : std::string("password");
    EXPECT_FALSE(copyWifiCredentialSnapshot(name, key, ssid, password));
    EXPECT_EQ(ssid, (std::array<char, 33>{}));
    EXPECT_EQ(password, (std::array<char, 65>{}));
  }
}
TEST(WifiCredentialSnapshotTest, ShortOutputDoesNotTruncateOrTouchCanaries) {
  std::array<char, 6> ssid{'x', 'x', 'x', 'x', 'x', 'x'}, password = ssid;
  EXPECT_FALSE(
      copyWifiCredentialSnapshot("name", "password", std::span(ssid).subspan(1, 4), std::span(password).subspan(1, 4)));
  EXPECT_EQ(ssid.front(), 'x');
  EXPECT_EQ(ssid.back(), 'x');
  EXPECT_EQ(password.front(), 'x');
  EXPECT_EQ(password.back(), 'x');
  EXPECT_EQ(password[1], '\0');
  EXPECT_FALSE(copyWifiCredentialSnapshot("name", "", {}, {}));
}

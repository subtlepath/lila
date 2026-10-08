#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "lib/Companion/CompanionIdentityUniqueness.h"
namespace {
struct Keys : companion::IdentityKeys {
  std::vector<uint32_t> values;
  uint32_t failAt = UINT32_MAX;
  uint32_t count() const override { return values.size(); }
  bool read(uint32_t index, uint32_t& identity) override {
    if (index == failAt) return false;
    identity = values[index];
    return true;
  }
};
}  // namespace
TEST(CompanionIdentityUniqueness, HandlesMultiplePartitionsAndUnalignedScratch) {
  Keys keys;
  keys.values.reserve(600);
  for (uint32_t value = 600; value != 0; --value) keys.values.push_back(0x12340000 + value);
  std::array<uint8_t, 513> bytes{};
  auto scratch = std::span(bytes).subspan(1);
  EXPECT_TRUE(companion::uniqueIdentityKeys(keys, scratch));
  keys.values.back() = keys.values.front();
  EXPECT_FALSE(companion::uniqueIdentityKeys(keys, scratch));
}
TEST(CompanionIdentityUniqueness, RejectsInvalidKeysAndReadFailure) {
  std::array<uint8_t, 512> scratch{};
  for (uint32_t value : {0U, UINT32_MAX}) {
    Keys keys;
    keys.values = {1, value, 2};
    EXPECT_FALSE(companion::uniqueIdentityKeys(keys, scratch));
  }
  Keys keys;
  keys.values = {1, 2, 3};
  keys.failAt = 1;
  EXPECT_FALSE(companion::uniqueIdentityKeys(keys, scratch));
  EXPECT_FALSE(companion::uniqueIdentityKeys(keys, std::span(scratch).first(3)));
}
TEST(CompanionIdentityUniqueness, EmptySourceAndSmallBucketsTerminate) {
  Keys keys;
  std::array<uint8_t, 4> scratch{};
  EXPECT_TRUE(companion::uniqueIdentityKeys(keys, scratch));
  keys.values = {0x12345671, 0x12345672, 0x12345673};
  EXPECT_TRUE(companion::uniqueIdentityKeys(keys, scratch));
  keys.values.push_back(keys.values[1]);
  EXPECT_FALSE(companion::uniqueIdentityKeys(keys, scratch));
}

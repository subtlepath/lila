#include <gtest/gtest.h>

#include <array>
#include <string>

#include "lib/Companion/CompanionZipNameNormalization.h"
using namespace companion;
namespace {
class ZipNameNormalizationTest : public testing::Test {
 protected:
  std::array<uint8_t, 3072> decoded{}, output{};
  std::array<uint32_t, 1024> inputScalars{};
  std::array<uint32_t, 4096> normalizedScalars{};
  bool normalize(const std::string& raw, bool utf8, bool directory, size_t& count) {
    return ZipNameNormalization::normalize(std::span(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()), utf8,
                                           directory, decoded, inputScalars, normalizedScalars, output, count);
  }
  std::string result(size_t count) { return {reinterpret_cast<const char*>(output.data()), count}; }
};
TEST_F(ZipNameNormalizationTest, Utf8Cp437AndDirectoryNamesHaveCanonicalIdentity) {
  size_t count = 99;
  ASSERT_TRUE(normalize("cafe\xcc\x81.ifo", true, false, count));
  EXPECT_EQ(result(count), "caf\xc3\xa9.ifo");
  ASSERT_TRUE(normalize("caf\x82.ifo", false, false, count));
  EXPECT_EQ(result(count), "caf\xc3\xa9.ifo");
  ASSERT_TRUE(normalize("cafe\xcc\x81/", true, true, count));
  EXPECT_EQ(result(count), "caf\xc3\xa9");
  ASSERT_TRUE(normalize("\xe1\x84\x80\xe1\x85\xa1/", true, true, count));
  EXPECT_EQ(result(count), "\xea\xb0\x80");
}
TEST_F(ZipNameNormalizationTest, UnsafeNamesAndCapacityFailuresPreserveOutput) {
  for (const std::string raw : {"../a", "/a", "a\\b", "a:b", "a//b", "a/", "\xc0\xaf"}) {
    output.fill(0xaa);
    size_t count = 99;
    EXPECT_FALSE(normalize(raw, true, false, count));
    EXPECT_EQ(count, 99u);
    for (auto byte : output) EXPECT_EQ(byte, 0xaa);
  }
  const std::array<uint8_t, 2> excluded{0xcd, 0x84};
  output.fill(0xaa);
  size_t count = 99;
  EXPECT_FALSE(ZipNameNormalization::normalize(excluded, true, false, decoded, inputScalars, normalizedScalars,
                                               std::span(output).first(3), count));
  EXPECT_EQ(count, 99u);
  for (auto byte : output) EXPECT_EQ(byte, 0xaa);
}
TEST_F(ZipNameNormalizationTest, MaximumDecodedBytesAndNormalizationGrowthAreDistinct) {
  size_t count = 99;
  EXPECT_TRUE(normalize(std::string(1024, 'a'), true, false, count));
  EXPECT_EQ(count, 1024u);
  EXPECT_FALSE(normalize(std::string(1025, 'a'), true, false, count));
  EXPECT_EQ(count, 1024u);
  std::string excluded;
  excluded.reserve(1024);
  for (unsigned i = 0; i < 512; ++i) excluded.append("\xcd\x84");
  ASSERT_TRUE(normalize(excluded, true, false, count));
  EXPECT_EQ(count, 2048u);
}
}  // namespace

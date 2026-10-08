#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "lib/Companion/CompanionUnicodeUtf8Nfc.h"
using namespace companion;
namespace {
class UnicodeUtf8NfcTest : public testing::Test {
 protected:
  std::array<uint32_t, 128> decoded{}, normalized{};
  std::array<uint8_t, 512> output{};
  bool convert(std::span<const uint8_t> bytes, size_t& count) {
    return UnicodeUtf8Nfc::normalize(bytes, decoded, normalized, output, count);
  }
};
TEST_F(UnicodeUtf8NfcTest, CanonicalReorderingSingletonHangulAndSupplementaryEncoding) {
  for (auto pair : {std::pair<std::string, std::string>{"cafe\xcc\x81", "caf\xc3\xa9"},
                    {"q\xcc\x87\xcc\xa3", "q\xcc\xa3\xcc\x87"},
                    {"\xe2\x84\xab", "\xc3\x85"},
                    {"\xe1\x84\x80\xe1\x85\xa1\xe1\x86\xa8", "\xea\xb0\x81"},
                    {"\xf0\x9f\x98\x80", "\xf0\x9f\x98\x80"}}) {
    size_t count = 99;
    ASSERT_TRUE(convert(std::span(reinterpret_cast<const uint8_t*>(pair.first.data()), pair.first.size()), count));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(output.data()), count), pair.second);
  }
}
TEST_F(UnicodeUtf8NfcTest, InvalidUtf8NeverWritesOutput) {
  for (auto bytes : {std::vector<uint8_t>{0x80},
                     {0xc0, 0xaf},
                     {0xe0, 0x80, 0xaf},
                     {0xed, 0xa0, 0x80},
                     {0xf4, 0x90, 0x80, 0x80},
                     {0xf0, 0x9f},
                     {0xc2, 'a'},
                     {0xff}}) {
    output.fill(0xaa);
    size_t count = 99;
    EXPECT_FALSE(convert(bytes, count));
    EXPECT_EQ(count, 99u);
    for (auto byte : output) EXPECT_EQ(byte, 0xaa);
  }
}
TEST_F(UnicodeUtf8NfcTest, CapacityFailuresPreserveOutputAndAllowRetry) {
  const std::array<uint8_t, 3> input{0xe2, 0x84, 0xab};
  output.fill(0xaa);
  size_t count = 99;
  EXPECT_FALSE(UnicodeUtf8Nfc::normalize(input, {}, normalized, output, count));
  EXPECT_FALSE(UnicodeUtf8Nfc::normalize(input, decoded, {}, output, count));
  EXPECT_FALSE(UnicodeUtf8Nfc::normalize(input, decoded, normalized, std::span(output).first(1), count));
  EXPECT_EQ(count, 99u);
  for (auto byte : output) EXPECT_EQ(byte, 0xaa);
  const std::array<uint8_t, 2> excluded{0xcd, 0x84};
  EXPECT_FALSE(UnicodeUtf8Nfc::normalize(excluded, decoded, normalized, std::span(output).first(3), count));
  EXPECT_EQ(count, 99u);
  ASSERT_TRUE(convert(excluded, count));
  EXPECT_EQ(count, 4u);
  EXPECT_EQ(output[0], 0xcc);
  EXPECT_EQ(output[1], 0x88);
  EXPECT_EQ(output[2], 0xcc);
  EXPECT_EQ(output[3], 0x81);
  EXPECT_TRUE(convert(input, count));
  EXPECT_EQ(count, 2u);
  EXPECT_TRUE(UnicodeUtf8Nfc::normalize({}, {}, {}, {}, count));
  EXPECT_EQ(count, 0u);
}
}  // namespace

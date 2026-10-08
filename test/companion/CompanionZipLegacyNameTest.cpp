#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionZipLegacyName.h"
#include "lib/Companion/CompanionZipPathValidation.h"
using namespace companion;
TEST(ZipLegacyNameTest, AllBytesMatchPublishedUnicodeMapping) {
  std::array<uint8_t, 256> input{};
  for (size_t i = 0; i < input.size(); ++i) input[i] = i;
  std::array<uint8_t, 768> output{};
  size_t count = 0;
  ASSERT_TRUE(zipCp437ToUtf8(input, output, count));
  std::ifstream file(ZIP_CP437_FIXTURE, std::ios::binary);
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(file), {}};
  ASSERT_EQ(count, expected.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), output.begin()));
}
TEST(ZipLegacyNameTest, CapacityFailureLeavesBytesAndCountUnchanged) {
  const std::array<uint8_t, 3> input{'a', 0x82, 0xb3};
  std::array<uint8_t, 6> output{};
  for (size_t size = 0; size < 6; ++size) {
    output.fill(0xaa);
    size_t count = 99;
    EXPECT_FALSE(zipCp437ToUtf8(input, std::span(output).first(size), count));
    EXPECT_EQ(count, 99u);
    for (auto byte : output) EXPECT_EQ(byte, 0xaa);
  }
  size_t count = 99;
  ASSERT_TRUE(zipCp437ToUtf8(input, output, count));
  EXPECT_EQ(count, 6u);
  ASSERT_TRUE(zipCp437ToUtf8({}, {}, count));
  EXPECT_EQ(count, 0u);
}
TEST(ZipLegacyNameTest, DecodedAccentsPassPathsAndControlsRemainUnsafe) {
  const std::array<uint8_t, 8> input{'c', 'a', 'f', 0x82, '.', 'i', 'f', 'o'};
  std::array<uint8_t, 24> output{};
  size_t count = 0;
  ASSERT_TRUE(zipCp437ToUtf8(input, output, count));
  ZipPathValidation grammar;
  ZipPathDetails details;
  ASSERT_TRUE(grammar.consume(std::span(output).first(count)));
  EXPECT_TRUE(grammar.finish(false, details));
  for (uint8_t control : {uint8_t{0}, uint8_t{31}, uint8_t{127}}) {
    const std::array<uint8_t, 2> unsafe{'a', control};
    ASSERT_TRUE(zipCp437ToUtf8(unsafe, output, count));
    grammar.reset();
    EXPECT_FALSE(grammar.consume(std::span(output).first(count)));
  }
}

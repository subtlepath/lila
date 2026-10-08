#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionBitmapFontValidation.h"
using namespace companion;
namespace {
class FontSource final : public InventoryIndexStorage {
 public:
  std::vector<uint8_t> bytes;
  unsigned reads = 0, failRead = 0;
  bool failSize = false;
  size_t maximumRead = 0;
  bool size(uint64_t& output) override {
    output = bytes.size();
    return !failSize;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    maximumRead = std::max(maximumRead, output.size());
    if (++reads == failRead || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
void little(std::vector<uint8_t>& bytes, size_t offset, uint32_t value, unsigned width) {
  for (unsigned index = 0; index < width; ++index) bytes.at(offset + index) = value >> (index * 8);
}
std::vector<uint8_t> font() {
  std::ifstream input(FONT_FIXTURE, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
class CompanionBitmapFontValidation : public testing::Test {
 protected:
  FontSource source;
  std::array<uint8_t, 32> scratch;
  BitmapFontValidation validator{source, scratch};
  void SetUp() override {
    source.bytes = font();
    ASSERT_EQ(source.bytes.size(), 142u);
  }
};
}  // namespace
TEST_F(CompanionBitmapFontValidation, ValidatesStylesGlyphsKernAndLigaturesWithBoundedReads) {
  BitmapFontDetails details;
  ASSERT_TRUE(validator.validate(details));
  EXPECT_EQ(details, (BitmapFontDetails{4, 1}));
  EXPECT_LE(source.maximumRead, 32u);
  source.bytes[10] = 1;
  ASSERT_TRUE(validator.validate(details));
  EXPECT_EQ(details, (BitmapFontDetails{4, 1}));
}
TEST_F(CompanionBitmapFontValidation, RejectsTruncationAtEveryBoundaryAndPreservesOutput) {
  for (size_t length = 0; length < 142; ++length) {
    source.bytes = font();
    source.bytes.resize(length);
    BitmapFontDetails details{99, 7};
    EXPECT_FALSE(validator.validate(details)) << length;
    EXPECT_EQ(details, (BitmapFontDetails{99, 7}));
  }
}
TEST_F(CompanionBitmapFontValidation, RejectsMalformedHeadersIntervalsGlyphsKernAndLigatures) {
  for (unsigned mutation = 0; mutation < 16; ++mutation) {
    source.bytes = font();
    switch (mutation) {
      case 0:
        source.bytes[0] ^= 1;
        break;
      case 1:
        source.bytes[8] = 5;
        break;
      case 2:
        source.bytes[10] = 2;
        break;
      case 3:
        source.bytes[12] = 0;
        break;
      case 4:
        source.bytes[12] = 5;
        break;
      case 5:
        source.bytes[32] = 4;
        break;
      case 6:
        little(source.bytes, 36, 4097, 4);
        break;
      case 7:
        little(source.bytes, 40, 65537, 4);
        break;
      case 8:
        little(source.bytes, 56, 32, 4);
        break;
      case 9:
        little(source.bytes, 68, 64, 4);
        break;
      case 10:
        little(source.bytes, 72, 1, 4);
        break;
      case 11:
        source.bytes[76] = 9;
        break;
      case 12:
        little(source.bytes, 88, UINT32_MAX, 4);
        break;
      case 13:
        source.bytes[126] = 0;
        break;
      case 14:
        little(source.bytes, 124, 68, 2);
        break;
      case 15:
        little(source.bytes, 135, 68, 4);
        break;
    }
    BitmapFontDetails details{99, 7};
    EXPECT_FALSE(validator.validate(details)) << mutation;
    EXPECT_EQ(details, (BitmapFontDetails{99, 7}));
  }
}
TEST_F(CompanionBitmapFontValidation, EveryReadFailureFailsClosedAndCanRetry) {
  BitmapFontDetails details;
  ASSERT_TRUE(validator.validate(details));
  const auto count = source.reads;
  for (unsigned failure = 1; failure <= count; ++failure) {
    source.reads = 0;
    source.failRead = failure;
    details = {99, 7};
    EXPECT_FALSE(validator.validate(details)) << failure;
    EXPECT_EQ(details, (BitmapFontDetails{99, 7}));
  }
  source.failRead = 0;
  EXPECT_TRUE(validator.validate(details));
  source.failSize = true;
  EXPECT_FALSE(validator.validate(details));
  BitmapFontValidation shortScratch(source, std::span(scratch).first(31));
  EXPECT_FALSE(shortScratch.validate(details));
}

TEST_F(CompanionBitmapFontValidation, ChecksAllStyleIdentitiesAndRejectsDuplicates) {
  const auto original = font();
  source.bytes.resize(original.size() + 96);
  std::copy_n(original.begin(), 32, source.bytes.begin());
  source.bytes[12] = 4;
  for (unsigned id = 0; id < 4; ++id) {
    std::copy_n(original.begin() + 32, 32, source.bytes.begin() + 32 + id * 32);
    source.bytes[32 + id * 32] = id;
    little(source.bytes, 56 + id * 32, 160, 4);
  }
  std::copy(original.begin() + 64, original.end(), source.bytes.begin() + 160);
  BitmapFontDetails details;
  ASSERT_TRUE(validator.validate(details));
  EXPECT_EQ(details.styles, 15u);
  source.bytes[96] = 1;
  EXPECT_FALSE(validator.validate(details));
}
TEST_F(CompanionBitmapFontValidation, SearchesMultipleIntervalsAndRejectsCoverageGaps) {
  source.bytes.insert(source.bytes.begin() + 76, 24, 0);
  little(source.bytes, 36, 3, 4);
  for (unsigned index = 0; index < 3; ++index) {
    const auto at = 64 + index * 12;
    little(source.bytes, at, 65 + index * 2, 4);
    little(source.bytes, at + 4, 65 + index * 2, 4);
    little(source.bytes, at + 8, index, 4);
  }
  little(source.bytes, 151, 67, 2);
  little(source.bytes, 155, (65u << 16) | 67u, 4);
  little(source.bytes, 159, 69, 4);
  BitmapFontDetails details;
  ASSERT_TRUE(validator.validate(details));
  little(source.bytes, 159, 68, 4);
  EXPECT_FALSE(validator.validate(details));
}

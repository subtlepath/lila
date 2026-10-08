#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionDictzipLayoutValidation.h"
using namespace companion;
namespace {
class Source final : public InventoryIndexStorage {
 public:
  std::vector<uint8_t> bytes;
  unsigned reads = 0, failRead = 0;
  bool size(uint64_t& out) override {
    out = bytes.size();
    return true;
  }
  bool read(uint64_t at, std::span<uint8_t> out) override {
    if (++reads == failRead || at > bytes.size() || out.size() > bytes.size() - at) return false;
    std::copy_n(bytes.begin() + at, out.size(), out.begin());
    return true;
  }
};
class CompanionDictzipLayoutValidation : public testing::Test {
 protected:
  Source source;
  std::array<uint8_t, 12> scratch;
  DictzipLayoutValidation validator{source, scratch};
  void SetUp() override {
    std::ifstream file(DICTZIP_FIXTURE, std::ios::binary);
    source.bytes = {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    ASSERT_GT(source.bytes.size(), 30u);
  }
};
}  // namespace
TEST_F(CompanionDictzipLayoutValidation, ReadsExistingIndependentChunkLayoutWithoutTableBuffer) {
  DictzipLayout layout;
  ASSERT_TRUE(validator.validate(layout));
  EXPECT_EQ(layout.expandedBytes, 120000u);
  EXPECT_EQ(layout.chunkLength, 58315u);
  EXPECT_EQ(layout.chunks, 3u);
  EXPECT_GT(layout.compressedBytes, 0u);
  EXPECT_GT(layout.dataOffset, layout.tableOffset);
}
TEST_F(CompanionDictzipLayoutValidation, RejectsWrongRandomAccessTableEvenWithValidGzipStream) {
  const auto original = source.bytes;
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    source.bytes = original;
    switch (mutation) {
      case 0:
        source.bytes[3] &= ~4;
        break;
      case 1:
        source.bytes[16] = 2;
        break;
      case 2:
        source.bytes[18] = 1;
        source.bytes[19] = 0;
        break;
      case 3:
        ++source.bytes[22];
        --source.bytes[24];
        break;
      case 4:
        source.bytes[20] = source.bytes[21] = 0;
        break;
      case 5:
        source.bytes[10] = source.bytes[11] = 255;
        break;
      case 6:
        source.bytes.back() = 255;
        break;
    }
    DictzipLayout layout;
    layout.expandedBytes = 99;
    EXPECT_FALSE(validator.validate(layout)) << mutation;
    EXPECT_EQ(layout.expandedBytes, 99u);
  }
}
TEST_F(CompanionDictzipLayoutValidation, EveryReadFailurePreservesOutputAndCanRetry) {
  DictzipLayout layout;
  ASSERT_TRUE(validator.validate(layout));
  const auto count = source.reads;
  for (unsigned failure = 1; failure <= count; ++failure) {
    source.reads = 0;
    source.failRead = failure;
    layout.expandedBytes = 99;
    EXPECT_FALSE(validator.validate(layout)) << failure;
    EXPECT_EQ(layout.expandedBytes, 99u);
  }
  source.failRead = 0;
  EXPECT_TRUE(validator.validate(layout));
  DictzipLayoutValidation shortScratch(source, std::span(scratch).first(11));
  EXPECT_FALSE(shortScratch.validate(layout));
  source.bytes.resize(19);
  EXPECT_FALSE(validator.validate(layout));
}
TEST_F(CompanionDictzipLayoutValidation, AllowsOtherExtraFieldsButRejectsDuplicateRandomAccessTable) {
  const auto original = source.bytes;
  const auto extra = uint32_t(original[10]) | uint32_t(original[11]) << 8;
  const std::array<uint8_t, 4> unknown{'X', 'Y', 0, 0};
  source.bytes.insert(source.bytes.begin() + 12 + extra, unknown.begin(), unknown.end());
  source.bytes[10] = (extra + 4) & 255;
  source.bytes[11] = (extra + 4) >> 8;
  DictzipLayout layout;
  ASSERT_TRUE(validator.validate(layout));
  EXPECT_EQ(layout.dataOffset, 12u + extra + 4);
  source.bytes = original;
  source.bytes.insert(source.bytes.begin() + 12 + extra, original.begin() + 12, original.begin() + 12 + extra);
  source.bytes[10] = (extra * 2) & 255;
  source.bytes[11] = (extra * 2) >> 8;
  EXPECT_FALSE(validator.validate(layout));
}
TEST_F(CompanionDictzipLayoutValidation, LayoutChecksDoNotSubstituteForDecodedCrcValidation) {
  DictzipLayout original;
  ASSERT_TRUE(validator.validate(original));
  source.bytes[source.bytes.size() - 8] ^= 1;
  DictzipLayout changed;
  ASSERT_TRUE(validator.validate(changed));
  EXPECT_NE(changed.crc, original.crc);
  EXPECT_EQ(changed.expandedBytes, original.expandedBytes);
}

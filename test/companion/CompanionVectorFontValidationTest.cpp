#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionVectorFontValidation.h"
using namespace companion;
namespace {
class FontSource final : public InventoryIndexStorage {
 public:
  std::vector<uint8_t> bytes;
  unsigned reads = 0, failRead = 0;
  bool failSize = false;
  bool size(uint64_t& output) override {
    output = bytes.size();
    return !failSize;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (++reads == failRead || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
std::vector<uint8_t> fixture(const char* name) {
  std::ifstream input(std::string(FONT_FIXTURE_DIR) + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void big(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte) bytes.at(at + byte) = value >> (24 - byte * 8);
}
class CompanionVectorFontValidation : public testing::Test {
 protected:
  FontSource source;
  std::array<uint8_t, 31> scratch;
  VectorFontValidation validator{source, scratch};
  void SetUp() override {
    source.bytes = fixture("VectorFont-sfnt.fixture");
    ASSERT_GT(source.bytes.size(), 140u);
  }
};
}  // namespace
TEST_F(CompanionVectorFontValidation, StreamsChecksumsWithOddWorkspaceAndHeadAdjustment) {
  VectorFontDetails details;
  ASSERT_TRUE(validator.validate(details));
  EXPECT_EQ(details.faces, 1u);
  for (size_t count : {size_t{16}, size_t{17}, size_t{20}, size_t{31}}) {
    VectorFontValidation another(source, std::span(scratch).first(count));
    EXPECT_TRUE(another.validate(details)) << count;
  }
  source.bytes.back() ^= 1;
  EXPECT_FALSE(validator.validate(details));
}
TEST_F(CompanionVectorFontValidation, ValidatesCollectionAndRejectsDuplicateOrUnalignedFaces) {
  source.bytes = fixture("VectorFont-collection.fixture");
  ASSERT_GT(source.bytes.size(), 20u);
  VectorFontDetails details;
  ASSERT_TRUE(validator.validate(details));
  EXPECT_EQ(details.faces, 2u);
  std::copy_n(source.bytes.begin() + 12, 4, source.bytes.begin() + 16);
  EXPECT_FALSE(validator.validate(details));
  source.bytes = fixture("VectorFont-collection.fixture");
  source.bytes[15] |= 1;
  EXPECT_FALSE(validator.validate(details));
  source.bytes = fixture("VectorFont-collection.fixture");
  big(source.bytes, 8, 257);
  EXPECT_FALSE(validator.validate(details));
}
TEST_F(CompanionVectorFontValidation, AllTruncationsAndReadFailuresPreserveOutput) {
  const auto original = source.bytes;
  for (size_t size = 0; size < original.size(); ++size) {
    source.bytes = original;
    source.bytes.resize(size);
    VectorFontDetails details{99};
    EXPECT_FALSE(validator.validate(details)) << size;
    EXPECT_EQ(details.faces, 99u);
  }
  source.bytes = original;
  source.reads = 0;
  VectorFontDetails details;
  ASSERT_TRUE(validator.validate(details));
  const auto reads = source.reads;
  for (unsigned failure = 1; failure <= reads; ++failure) {
    source.reads = 0;
    source.failRead = failure;
    details = {99};
    EXPECT_FALSE(validator.validate(details)) << failure;
    EXPECT_EQ(details.faces, 99u);
  }
  source.failRead = 0;
  EXPECT_TRUE(validator.validate(details));
  source.failSize = true;
  EXPECT_FALSE(validator.validate(details));
  VectorFontValidation shortScratch(source, std::span(scratch).first(15));
  EXPECT_FALSE(shortScratch.validate(details));
}
TEST_F(CompanionVectorFontValidation, RejectsVersionsTagsOffsetsLengthsAndMissingTables) {
  const auto original = source.bytes;
  for (unsigned mutation = 0; mutation < 8; ++mutation) {
    source.bytes = original;
    switch (mutation) {
      case 0:
        big(source.bytes, 0, 0x20000);
        break;
      case 1:
        source.bytes[4] = source.bytes[5] = 0;
        break;
      case 2:
        source.bytes[12] = 0;
        break;
      case 3:
        std::copy_n(source.bytes.begin() + 12, 4, source.bytes.begin() + 28);
        break;
      case 4:
        big(source.bytes, 20, UINT32_MAX);
        break;
      case 5:
        big(source.bytes, 24, UINT32_MAX);
        break;
      case 6:
        big(source.bytes, 12, 0x63616161);
        break;
      case 7:
        big(source.bytes, 0, 0x4f54544f);
        break;
    }
    VectorFontDetails details{99};
    EXPECT_FALSE(validator.validate(details)) << mutation;
    EXPECT_EQ(details.faces, 99u);
  }
}

TEST_F(CompanionVectorFontValidation, ValidatesCffAndActualRepositoryFonts) {
  source.bytes = fixture("VectorFont-otto.fixture");
  VectorFontDetails details;
  ASSERT_TRUE(validator.validate(details));
  EXPECT_EQ(details.faces, 1u);
  for (const char* name :
       {"NotoSerif/NotoSerif-Bold.ttf", "NotoSansArabic/NotoSansArabic-Regular.ttf", "Ubuntu/Ubuntu-Regular.ttf"}) {
    std::ifstream input(std::string(REPO_FONT_DIR) + name, std::ios::binary);
    source.bytes = {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    ASSERT_GT(source.bytes.size(), 140u) << name;
    ASSERT_TRUE(validator.validate(details)) << name;
    EXPECT_EQ(details.faces, 1u);
  }
}

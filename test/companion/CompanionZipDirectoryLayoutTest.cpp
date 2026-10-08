#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionZipDirectoryLayout.h"
using namespace companion;
namespace {
struct Source : InventoryIndexStorage {
  std::vector<uint8_t> bytes;
  unsigned reads = 0, failRead = 0, sizes = 0;
  bool changed = false;
  bool size(uint64_t& output) override {
    output = bytes.size() + (changed && ++sizes > 1);
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (++reads == failRead || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
class ZipDirectoryLayoutTest : public testing::Test {
 protected:
  Source source;
  std::array<uint8_t, 512> scratch;
  void SetUp() override {
    std::ifstream file(ZIP_LAYOUT_FIXTURE, std::ios::binary);
    source.bytes = {std::istreambuf_iterator<char>(file), {}};
  }
  void comment(size_t bytes) {
    inventory_detail::write(source.bytes, source.bytes.size() - 2, bytes, 2);
    source.bytes.insert(source.bytes.end(), bytes, 'x');
  }
  void zip64() {
    const auto end = source.bytes.size() - 22;
    const auto count = inventory_detail::read(source.bytes, end + 10, 2);
    const auto length = inventory_detail::read(source.bytes, end + 12, 4);
    const auto offset = inventory_detail::read(source.bytes, end + 16, 4);
    std::array<uint8_t, 76> tail{};
    inventory_detail::write(tail, 0, 0x06064b50, 4);
    inventory_detail::write(tail, 4, 44, 8);
    inventory_detail::write(tail, 12, 45, 2);
    inventory_detail::write(tail, 14, 45, 2);
    inventory_detail::write(tail, 24, count, 8);
    inventory_detail::write(tail, 32, count, 8);
    inventory_detail::write(tail, 40, length, 8);
    inventory_detail::write(tail, 48, offset, 8);
    inventory_detail::write(tail, 56, 0x07064b50, 4);
    inventory_detail::write(tail, 64, end, 8);
    inventory_detail::write(tail, 72, 1, 4);
    source.bytes.insert(source.bytes.begin() + end, tail.begin(), tail.end());
    const auto at = end + 76;
    inventory_detail::write(source.bytes, at + 8, 65535, 2);
    inventory_detail::write(source.bytes, at + 10, 65535, 2);
    inventory_detail::write(source.bytes, at + 12, UINT32_MAX, 4);
    inventory_detail::write(source.bytes, at + 16, UINT32_MAX, 4);
  }
};
TEST_F(ZipDirectoryLayoutTest, FullCommentTailAndAllBankBoundariesUseBoundedScratch) {
  for (size_t commentBytes : {size_t{0}, size_t{43}, size_t{1024}, size_t{65535}}) {
    SetUp();
    comment(commentBytes);
    for (size_t width = 64; width <= 512; ++width) {
      ZipDirectoryLayoutValidation validation(source, std::span(scratch).first(width));
      ZipDirectoryLayout layout;
      ASSERT_TRUE(validation.validate(layout)) << width << ":" << commentBytes;
      EXPECT_EQ(layout.entries, 4u);
      EXPECT_FALSE(layout.zip64);
      EXPECT_EQ(layout.archiveBytes, source.bytes.size());
    }
  }
}
TEST_F(ZipDirectoryLayoutTest, Zip64LocatorAndRecordAreCheckedIncludingNonsentinelAgreement) {
  zip64();
  ZipDirectoryLayoutValidation validation(source, scratch);
  ZipDirectoryLayout layout;
  ASSERT_TRUE(validation.validate(layout));
  EXPECT_TRUE(layout.zip64);
  EXPECT_EQ(layout.entries, 4u);
  const auto end = source.bytes.size() - 22;
  inventory_detail::write(source.bytes, end + 8, 4, 2);
  inventory_detail::write(source.bytes, end + 10, 4, 2);
  inventory_detail::write(source.bytes, end + 12, layout.centralBytes, 4);
  inventory_detail::write(source.bytes, end + 16, layout.centralOffset, 4);
  ASSERT_TRUE(validation.validate(layout));
  inventory_detail::write(source.bytes, end + 10, 3, 2);
  EXPECT_FALSE(validation.validate(layout));
}
TEST_F(ZipDirectoryLayoutTest, MultidiskConflictingCountsOverflowAndBadCommentsPreserveOutput) {
  const auto original = source.bytes;
  for (unsigned invalid = 0; invalid < 7; ++invalid) {
    source.bytes = original;
    const auto end = source.bytes.size() - 22;
    if (invalid == 0) source.bytes[end + 4] = 1;
    if (invalid == 1) source.bytes[end + 8] = 3;
    if (invalid == 2) inventory_detail::write(source.bytes, end + 16, UINT32_MAX, 4);
    if (invalid == 3) inventory_detail::write(source.bytes, end + 12, UINT32_MAX, 4);
    if (invalid == 4) source.bytes[end + 20] = 1;
    if (invalid == 5) source.bytes.push_back(0);
    if (invalid == 6) inventory_detail::write(source.bytes, end + 16, end, 4);
    ZipDirectoryLayoutValidation validation(source, scratch);
    ZipDirectoryLayout layout;
    layout.entries = 99;
    EXPECT_FALSE(validation.validate(layout)) << invalid;
    EXPECT_EQ(layout.entries, 99u);
  }
}
TEST_F(ZipDirectoryLayoutTest, EveryReadFailureCancellationSizeChangeAndEntryLimitCanRetry) {
  zip64();
  ZipDirectoryLayoutValidation validation(source, scratch);
  ZipDirectoryLayout layout;
  ASSERT_TRUE(validation.validate(layout));
  const auto reads = source.reads;
  for (unsigned failure = 1; failure <= reads; ++failure) {
    source.reads = 0;
    source.failRead = failure;
    layout.entries = 99;
    EXPECT_FALSE(validation.validate(layout));
    EXPECT_EQ(layout.entries, 99u);
  }
  source.failRead = 0;
  ASSERT_TRUE(validation.validate(layout));
  ZipDirectoryLayoutValidation cancelled(source, scratch, 20000, [](void*) { return false; });
  EXPECT_FALSE(cancelled.validate(layout));
  ZipDirectoryLayoutValidation limited(source, scratch, 3);
  EXPECT_FALSE(limited.validate(layout));
  source.changed = true;
  source.sizes = 0;
  EXPECT_FALSE(validation.validate(layout));
}
TEST_F(ZipDirectoryLayoutTest, MalformedZip64RecordsNeverAssignOutput) {
  zip64();
  const auto original = source.bytes;
  for (unsigned invalid = 0; invalid < 11; ++invalid) {
    source.bytes = original;
    const auto end = source.bytes.size() - 22, locator = end - 20, record = locator - 56;
    if (invalid == 0) inventory_detail::write(source.bytes, locator + 4, 1, 4);
    if (invalid == 1) inventory_detail::write(source.bytes, locator + 16, 2, 4);
    if (invalid == 2) inventory_detail::write(source.bytes, locator + 8, UINT64_MAX, 8);
    if (invalid == 3) inventory_detail::write(source.bytes, record + 4, 43, 8);
    if (invalid == 4) inventory_detail::write(source.bytes, record + 16, 1, 4);
    if (invalid == 5) inventory_detail::write(source.bytes, record + 24, 3, 8);
    if (invalid == 6) inventory_detail::write(source.bytes, record + 48, UINT64_MAX, 8);
    if (invalid == 7) inventory_detail::write(source.bytes, record + 40, UINT64_MAX, 8);
    if (invalid == 8) inventory_detail::write(source.bytes, record + 14, 44, 2);
    if (invalid == 9) source.bytes[record] = 0;
    if (invalid == 10) source.bytes[locator] = 0;
    ZipDirectoryLayoutValidation validation(source, scratch);
    ZipDirectoryLayout output;
    output.entries = 99;
    EXPECT_FALSE(validation.validate(output)) << invalid;
    EXPECT_EQ(output.entries, 99u);
  }
}
TEST_F(ZipDirectoryLayoutTest, ExtensibleZip64RecordsAndEmptyZipEndMetadataAreSupported) {
  zip64();
  const auto locator = source.bytes.size() - 22 - 20, record = locator - 56;
  source.bytes.insert(source.bytes.begin() + locator, 8, 42);
  inventory_detail::write(source.bytes, record + 4, 52, 8);
  ZipDirectoryLayoutValidation validation(source, scratch);
  ZipDirectoryLayout output;
  ASSERT_TRUE(validation.validate(output));
  EXPECT_TRUE(output.zip64);
  EXPECT_EQ(output.entries, 4u);
  source.bytes.assign(22, 0);
  inventory_detail::write(source.bytes, 0, 0x06054b50, 4);
  ASSERT_TRUE(validation.validate(output));
  EXPECT_EQ(output.entries, 0u);
  EXPECT_EQ(output.centralBytes, 0u);
  EXPECT_FALSE(output.zip64);
}
}  // namespace

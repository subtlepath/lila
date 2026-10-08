#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionZipEntryMetadataValidation.h"
using namespace companion;
namespace {
struct Source : InventoryIndexStorage {
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
class ZipEntryMetadataTest : public testing::Test {
 protected:
  Source source;
  std::array<uint8_t, 64> scratch;
  ZipDirectoryLayout layout;
  void SetUp() override {
    std::ifstream file(ZIP_METADATA_FIXTURE, std::ios::binary);
    source.bytes = {std::istreambuf_iterator<char>(file), {}};
    ZipDirectoryLayoutValidation parser(source, scratch);
    ASSERT_TRUE(parser.validate(layout));
    source.reads = 0;
  }
};
TEST_F(ZipEntryMetadataTest, AllCanonicalMembersHaveBoundedPayloadAndMatchingNames) {
  ZipEntryMetadataValidation parser(source, scratch);
  auto at = layout.centralOffset;
  uint64_t priorEnd = 0;
  for (uint64_t i = 0; i < layout.entries; ++i) {
    ZipEntryMetadata entry;
    ASSERT_TRUE(parser.validate(layout, at, entry));
    EXPECT_EQ(entry.localOffset, priorEnd);
    EXPECT_LT(entry.payload.offset, entry.localEnd);
    EXPECT_LE(entry.localEnd, layout.centralOffset);
    EXPECT_FALSE(entry.directory);
    priorEnd = entry.localEnd;
    at = entry.nextCentralOffset;
  }
  EXPECT_EQ(at, layout.centralOffset + layout.centralBytes);
  EXPECT_EQ(priorEnd, layout.centralOffset);
}
TEST_F(ZipEntryMetadataTest, LocalMismatchAndCentralRangeCorruptionFailWithoutOutput) {
  const auto original = source.bytes;
  for (size_t at :
       {size_t{0}, size_t{4}, size_t{6}, size_t{8}, size_t{14}, size_t{18}, size_t{22}, size_t{26}, size_t{30}}) {
    source.bytes = original;
    source.bytes[at] ^= 1;
    ZipEntryMetadataValidation parser(source, scratch);
    ZipEntryMetadata out;
    out.localEnd = 99;
    EXPECT_FALSE(parser.validate(layout, layout.centralOffset, out)) << at;
    EXPECT_EQ(out.localEnd, 99u);
  }
  source.bytes = original;
  inventory_detail::write(source.bytes, layout.centralOffset + 20, UINT32_MAX, 4);
  ZipEntryMetadataValidation parser(source, scratch);
  ZipEntryMetadata out;
  EXPECT_FALSE(parser.validate(layout, layout.centralOffset, out));
}
TEST_F(ZipEntryMetadataTest, EveryReadFailureAndCancellationPreserveOutput) {
  ZipEntryMetadataValidation parser(source, scratch);
  ZipEntryMetadata out;
  ASSERT_TRUE(parser.validate(layout, layout.centralOffset, out));
  const auto reads = source.reads;
  for (unsigned fail = 1; fail <= reads; ++fail) {
    source.reads = 0;
    source.failRead = fail;
    out.localEnd = 99;
    EXPECT_FALSE(parser.validate(layout, layout.centralOffset, out));
    EXPECT_EQ(out.localEnd, 99u);
  }
  source.failRead = 0;
  ZipEntryMetadataValidation cancelled(source, scratch, [](void*) { return false; });
  EXPECT_FALSE(cancelled.validate(layout, layout.centralOffset, out));
  EXPECT_EQ(out.localEnd, 99u);
}
TEST_F(ZipEntryMetadataTest, RealDeflateZip64LocalHeadersAndStreamingDescriptors) {
  for (const char* name : {"seekable", "streamed"}) {
    std::ifstream file(std::string(ZIP_METADATA_DIR) + "/ZipEntry-zip64-" + name + ".fixture", std::ios::binary);
    source.bytes = {std::istreambuf_iterator<char>(file), {}};
    ZipDirectoryLayoutValidation endParser(source, scratch);
    ASSERT_TRUE(endParser.validate(layout));
    ZipEntryMetadataValidation parser(source, scratch);
    ZipEntryMetadata entry;
    ASSERT_TRUE(parser.validate(layout, layout.centralOffset, entry)) << name;
    EXPECT_TRUE(entry.zip64);
    EXPECT_EQ(entry.payload.method, 8u);
    EXPECT_EQ(entry.payload.expandedBytes, 120000u);
    EXPECT_EQ(entry.payload.crc, 0x7146dd0bu);
    EXPECT_EQ(entry.localEnd, layout.centralOffset);
  }
}

}  // namespace

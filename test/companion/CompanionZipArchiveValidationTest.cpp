#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionZipArchiveValidation.h"
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
  void load(const char* name) {
    std::ifstream file(std::string(ZIP_ARCHIVE_DIR) + "/" + name, std::ios::binary);
    bytes = {std::istreambuf_iterator<char>(file), {}};
    reads = 0;
  }
};
class ZipArchiveTest : public testing::Test {
 protected:
  Source source;
  std::array<uint8_t, 64> scratch{};
  tinfl_decompressor decoder{};
  std::array<uint8_t, 32768> window{};
  void SetUp() override { source.load("DictionaryBundle-plain.fixture"); }
};
TEST_F(ZipArchiveTest, CanonicalAndRealZip64DeflateArchivesPassCompletePayloadSweep) {
  for (const char* name :
       {"DictionaryBundle-plain.fixture", "ZipEntry-zip64-seekable.fixture", "ZipEntry-zip64-streamed.fixture"}) {
    source.load(name);
    ZipArchiveValidation parser(source, scratch, &decoder, window);
    ZipDirectoryLayout output;
    ASSERT_TRUE(parser.validate(output)) << name;
    EXPECT_GT(output.entries, 0u);
  }
}
TEST_F(ZipArchiveTest, UnderreportedDirectoryCountAndPayloadCorruptionAreRejected) {
  const auto original = source.bytes;
  const auto end = source.bytes.size() - 22;
  inventory_detail::write(source.bytes, end + 8, 3, 2);
  inventory_detail::write(source.bytes, end + 10, 3, 2);
  ZipArchiveValidation parser(source, scratch, &decoder, window);
  ZipDirectoryLayout output;
  output.entries = 99;
  EXPECT_FALSE(parser.validate(output));
  EXPECT_EQ(output.entries, 99u);
  source.bytes = original;
  ZipDirectoryLayoutValidation layoutParser(source, scratch);
  ZipDirectoryLayout layout;
  ASSERT_TRUE(layoutParser.validate(layout));
  ZipEntryMetadataValidation entryParser(source, scratch);
  ZipEntryMetadata entry;
  ASSERT_TRUE(entryParser.validate(layout, layout.centralOffset, entry));
  source.bytes[entry.payload.offset] ^= 1;
  EXPECT_FALSE(parser.validate(output));
  EXPECT_EQ(output.entries, 99u);
}
TEST_F(ZipArchiveTest, EveryReadFailureAndEveryCancellationPointLeaveResultUnchanged) {
  ZipArchiveValidation parser(source, scratch, &decoder, window);
  ZipDirectoryLayout output;
  ASSERT_TRUE(parser.validate(output));
  const auto reads = source.reads;
  for (unsigned fail = 1; fail <= reads; ++fail) {
    source.reads = 0;
    source.failRead = fail;
    output.entries = 99;
    EXPECT_FALSE(parser.validate(output));
    EXPECT_EQ(output.entries, 99u);
  }
  source.failRead = 0;
  struct Control {
    unsigned calls = 0, fail = 0;
  } control;
  auto tick = [](void* ptr) {
    auto& state = *static_cast<Control*>(ptr);
    return ++state.calls != state.fail;
  };
  ZipArchiveValidation cancellable(source, scratch, &decoder, window, tick, &control);
  ASSERT_TRUE(cancellable.validate(output));
  const auto calls = control.calls;
  for (unsigned fail = 1; fail <= calls; ++fail) {
    control.calls = 0;
    control.fail = fail;
    output.entries = 99;
    EXPECT_FALSE(cancellable.validate(output));
    EXPECT_EQ(output.entries, 99u);
  }
  control.fail = 0;
  EXPECT_TRUE(cancellable.validate(output));
}
TEST_F(ZipArchiveTest, OptionalSignatureRecordHasExactBoundedFraming) {
  const auto original = source.bytes;
  for (size_t count : {size_t{0}, size_t{1}, size_t{65535}}) {
    source.bytes = original;
    const auto end = source.bytes.size() - 22;
    const auto centralBytes = inventory_detail::read(source.bytes, end + 12, 4);
    source.bytes.insert(source.bytes.begin() + end, count + 6, 0);
    inventory_detail::write(source.bytes, end, 0x05054b50, 4);
    inventory_detail::write(source.bytes, end + 4, count, 2);
    inventory_detail::write(source.bytes, end + count + 6 + 12, centralBytes + count + 6, 4);
    ZipArchiveValidation parser(source, scratch, &decoder, window);
    ZipDirectoryLayout output;
    ASSERT_TRUE(parser.validate(output)) << count;
    output.entries = 99;
    source.bytes[end] ^= 1;
    EXPECT_FALSE(parser.validate(output));
    EXPECT_EQ(output.entries, 99u);
    source.bytes[end] ^= 1;
    inventory_detail::write(source.bytes, end + 4, count ? count - 1 : 1, 2);
    EXPECT_FALSE(parser.validate(output));
    EXPECT_EQ(output.entries, 99u);
  }
}

TEST_F(ZipArchiveTest, RangeAuditRejectsDuplicateLocalRecordAfterPayloadChecksPass) {
  struct Stage : ZipRangeStorage {
    std::vector<uint8_t> bytes;
    bool reset() override {
      bytes.clear();
      return true;
    }
    bool read(uint64_t at, std::span<uint8_t> out) override {
      if (at > bytes.size() || out.size() > bytes.size() - at) return false;
      std::copy_n(bytes.begin() + at, out.size(), out.begin());
      return true;
    }
    bool write(uint64_t at, std::span<const uint8_t> in) override {
      bytes.resize(std::max<uint64_t>(bytes.size(), at + in.size()));
      std::copy(in.begin(), in.end(), bytes.begin() + at);
      return true;
    }
    bool seal(uint64_t size) override { return size == bytes.size(); }
  } stage;
  ZipRangeValidation ranges(stage);
  ZipArchiveValidation parser(source, scratch, &decoder, window);
  ZipDirectoryLayout layout;
  ASSERT_TRUE(parser.validate(layout, &ranges));
  ZipEntryMetadataValidation entries(source, scratch);
  ZipEntryMetadata entry;
  ASSERT_TRUE(entries.validate(layout, layout.centralOffset, entry));
  const auto recordBytes = entry.nextCentralOffset - layout.centralOffset;
  std::vector<uint8_t> duplicate(source.bytes.begin() + layout.centralOffset,
                                 source.bytes.begin() + entry.nextCentralOffset);
  const auto end = source.bytes.size() - 22;
  source.bytes.insert(source.bytes.begin() + end, duplicate.begin(), duplicate.end());
  inventory_detail::write(source.bytes, end + recordBytes + 8, layout.entries + 1, 2);
  inventory_detail::write(source.bytes, end + recordBytes + 10, layout.entries + 1, 2);
  inventory_detail::write(source.bytes, end + recordBytes + 12, layout.centralBytes + recordBytes, 4);
  ASSERT_TRUE(parser.validate(layout));
  layout.entries = 99;
  EXPECT_FALSE(parser.validate(layout, &ranges));
  EXPECT_EQ(layout.entries, 99u);
  EXPECT_FALSE(ranges.finish());
}

TEST_F(ZipArchiveTest, MatchingUnsafeLocalAndCentralNamesAreRejected) {
  ZipDirectoryLayoutValidation layoutParser(source, scratch);
  ZipDirectoryLayout layout;
  ASSERT_TRUE(layoutParser.validate(layout));
  ZipEntryMetadataValidation entryParser(source, scratch);
  ZipEntryMetadata entry;
  ASSERT_TRUE(entryParser.validate(layout, layout.centralOffset, entry));
  const auto original = source.bytes;
  for (uint8_t unsafe : {uint8_t{'/'}, uint8_t{'\\'}, uint8_t{':'}, uint8_t{0}, uint8_t{31}}) {
    source.bytes = original;
    source.bytes[entry.nameOffset] = unsafe;
    source.bytes[entry.localOffset + 30] = unsafe;
    ZipArchiveValidation parser(source, scratch, &decoder, window);
    ZipDirectoryLayout out;
    out.entries = 99;
    EXPECT_FALSE(parser.validate(out));
    EXPECT_EQ(out.entries, 99u);
  }
}

TEST_F(ZipArchiveTest, AggregateExpandedLimitAcceptsExactBoundaryAndRejectsOneByteLess) {
  ZipDirectoryLayoutValidation layoutParser(source, scratch);
  ZipDirectoryLayout layout;
  ASSERT_TRUE(layoutParser.validate(layout));
  ZipEntryMetadataValidation metadata(source, scratch);
  uint64_t total = 0, at = layout.centralOffset;
  for (uint64_t i = 0; i < layout.entries; ++i) {
    ZipEntryMetadata entry;
    ASSERT_TRUE(metadata.validate(layout, at, entry));
    total += entry.payload.expandedBytes;
    at = entry.nextCentralOffset;
  }
  ASSERT_GT(total, 0u);
  ZipArchiveValidation exact(source, scratch, &decoder, window, nullptr, nullptr, total);
  ZipArchiveValidation insufficient(source, scratch, &decoder, window, nullptr, nullptr, total - 1);
  ZipArchiveValidation zero(source, scratch, &decoder, window, nullptr, nullptr, 0);
  ZipDirectoryLayout output;
  ASSERT_TRUE(exact.validate(output));
  output.entries = 99;
  EXPECT_FALSE(insufficient.validate(output));
  EXPECT_EQ(output.entries, 99u);
  EXPECT_FALSE(zero.validate(output));
  EXPECT_EQ(output.entries, 99u);
  EXPECT_TRUE(exact.validate(output));
}

}  // namespace

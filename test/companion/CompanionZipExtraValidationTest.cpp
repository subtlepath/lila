#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "lib/Companion/CompanionZipExtraValidation.h"
using namespace companion;
namespace {
struct Source : InventoryIndexStorage {
  std::vector<uint8_t> bytes;
  unsigned reads = 0, failRead = 0, sizes = 0;
  bool changed = false;
  bool size(uint64_t& out) override {
    out = bytes.size() + (changed && ++sizes > 1);
    return true;
  }
  bool read(uint64_t at, std::span<uint8_t> out) override {
    if (++reads == failRead || at > bytes.size() || out.size() > bytes.size() - at) return false;
    std::copy_n(bytes.begin() + at, out.size(), out.begin());
    return true;
  }
  void field(uint16_t id, std::span<const uint8_t> data) {
    auto at = bytes.size();
    bytes.resize(at + 4 + data.size());
    inventory_detail::write(bytes, at, id, 2);
    inventory_detail::write(bytes, at + 2, data.size(), 2);
    std::copy(data.begin(), data.end(), bytes.begin() + at + 4);
  }
};
TEST(ZipExtraValidationTest, EveryCentralSentinelCombinationUsesFixedConditionalOrder) {
  for (unsigned mask = 0; mask < 16; ++mask) {
    Source source;
    std::array<uint8_t, 28> scratch{}, data{};
    ZipExtraValues header{3, 4, 5, 0}, expected = header;
    size_t at = 0;
    if (mask & 1) {
      header.expandedBytes = UINT32_MAX;
      expected.expandedBytes = 0x100000003ULL;
      inventory_detail::write(data, at, expected.expandedBytes, 8);
      at += 8;
    }
    if (mask & 2) {
      header.compressedBytes = UINT32_MAX;
      expected.compressedBytes = 0x100000004ULL;
      inventory_detail::write(data, at, expected.compressedBytes, 8);
      at += 8;
    }
    if (mask & 4) {
      header.localOffset = UINT32_MAX;
      expected.localOffset = 0x100000005ULL;
      inventory_detail::write(data, at, expected.localOffset, 8);
      at += 8;
    }
    if (mask & 8) {
      header.disk = UINT16_MAX;
      expected.disk = 7;
      inventory_detail::write(data, at, 7, 4);
      at += 4;
    }
    source.field(0xabcd, std::span(data).first(3));
    source.field(1, std::span(data).first(at));
    expected.zip64 = true;
    ZipExtraValues output;
    ZipExtraValidation parser(source, scratch);
    ASSERT_TRUE(parser.validate(0, source.bytes.size(), false, header, output)) << mask;
    EXPECT_EQ(output, expected);
  }
}
TEST(ZipExtraValidationTest, LocalRequiresBothSizesAndChecksUnsaturatedValues) {
  Source source;
  std::array<uint8_t, 28> scratch{}, data{};
  inventory_detail::write(data, 0, 3, 8);
  inventory_detail::write(data, 8, 4, 8);
  source.field(1, std::span(data).first(16));
  ZipExtraValidation parser(source, scratch);
  ZipExtraValues output, header{UINT32_MAX, 4, 0, 0};
  ASSERT_TRUE(parser.validate(0, 20, true, header, output));
  EXPECT_EQ(output.expandedBytes, 3u);
  header.compressedBytes = 5;
  EXPECT_FALSE(parser.validate(0, 20, true, header, output));
  source.bytes.resize(12);
  inventory_detail::write(source.bytes, 2, 8, 2);
  EXPECT_FALSE(parser.validate(0, 12, true, header, output));
}
TEST(ZipExtraValidationTest, MalformedFramingMissingAndDuplicateZip64PreserveOutput) {
  std::array<uint8_t, 28> scratch{}, data{};
  const ZipExtraValues sentinel{UINT32_MAX, 0, 0, 0}, saved{11, 12, 13, 14, true};
  for (unsigned fault = 0; fault < 6; ++fault) {
    Source source;
    if (fault == 0) source.bytes = {0};
    if (fault == 1) source.bytes = {2, 0, 1, 0};
    if (fault == 2) source.field(2, {});
    if (fault == 3) source.field(1, std::span(data).first(7));
    if (fault == 4) source.field(1, std::span(data).first(9));
    if (fault == 5) {
      source.field(1, std::span(data).first(8));
      source.field(1, std::span(data).first(8));
    }
    ZipExtraValues output = saved;
    ZipExtraValidation parser(source, scratch);
    EXPECT_FALSE(parser.validate(0, source.bytes.size(), false, sentinel, output)) << fault;
    EXPECT_EQ(output, saved);
  }
}
TEST(ZipExtraValidationTest, EveryReadFailureCancellationAndChangingExtentFailClosed) {
  Source source;
  std::array<uint8_t, 28> scratch{}, data{};
  source.field(2, std::span(data).first(5));
  source.field(1, std::span(data).first(8));
  ZipExtraValues header{UINT32_MAX, 0, 0, 0}, output;
  ZipExtraValidation parser(source, scratch);
  ASSERT_TRUE(parser.validate(0, source.bytes.size(), false, header, output));
  const auto reads = source.reads;
  for (unsigned fail = 1; fail <= reads; ++fail) {
    source.reads = 0;
    source.failRead = fail;
    output = header;
    EXPECT_FALSE(parser.validate(0, source.bytes.size(), false, header, output));
    EXPECT_EQ(output, header);
  }
  source.failRead = 0;
  source.changed = true;
  source.sizes = 0;
  EXPECT_FALSE(parser.validate(0, source.bytes.size(), false, header, output));
  source.changed = false;
  ZipExtraValidation cancelled(source, scratch, [](void*) { return false; });
  EXPECT_FALSE(cancelled.validate(0, source.bytes.size(), false, header, output));
  EXPECT_FALSE(parser.validate(UINT64_MAX, 1, false, header, output));
}
}  // namespace

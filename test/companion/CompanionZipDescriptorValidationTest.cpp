#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "lib/Companion/CompanionZipDescriptorValidation.h"
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
  void descriptor(bool wide, bool signature, uint32_t crc, uint64_t compressed, uint64_t expanded) {
    const unsigned width = wide ? 8 : 4;
    bytes.assign(3 + 4 + width * 2 + (signature ? 4 : 0), 0);
    size_t at = 3;
    if (signature) {
      inventory_detail::write(bytes, at, 0x08074b50, 4);
      at += 4;
    }
    inventory_detail::write(bytes, at, crc, 4);
    inventory_detail::write(bytes, at + 4, compressed, width);
    inventory_detail::write(bytes, at + 4 + width, expanded, width);
  }
};
TEST(ZipDescriptorValidationTest, SignedAndUnsignedWidthsAndSignatureValuedCrc) {
  std::array<uint8_t, 24> scratch{};
  for (bool wide : {false, true})
    for (bool signature : {false, true})
      for (uint32_t crc : {0u, 0x08074b50u, 0xdeadbeefu}) {
        Source source;
        const uint64_t compressed = wide ? 0x100000005ULL : 5, expanded = wide ? 0x200000007ULL : 7;
        source.descriptor(wide, signature, crc, compressed, expanded);
        const auto expected = source.bytes.size();
        source.bytes.resize(expected + 30, 0);
        ZipDescriptorValidation parser(source, scratch);
        uint64_t end = 99;
        ASSERT_TRUE(parser.validate(3, source.bytes.size(), wide, crc, compressed, expanded, end));
        EXPECT_EQ(end, expected);
      }
}
TEST(ZipDescriptorValidationTest, EveryDescriptorByteCorruptionAndTruncationPreservesOutput) {
  std::array<uint8_t, 24> scratch{};
  for (bool wide : {false, true})
    for (bool signature : {false, true}) {
      Source original;
      original.descriptor(wide, signature, 0xdeadbeef, 5, 7);
      for (size_t at = 3; at < original.bytes.size(); ++at) {
        Source source = original;
        source.bytes[at] ^= 1;
        ZipDescriptorValidation parser(source, scratch);
        uint64_t end = 99;
        EXPECT_FALSE(parser.validate(3, source.bytes.size(), wide, 0xdeadbeef, 5, 7, end));
        EXPECT_EQ(end, 99u);
      }
      for (size_t size = 3; size < original.bytes.size(); ++size) {
        Source source = original;
        source.bytes.resize(size);
        ZipDescriptorValidation parser(source, scratch);
        uint64_t end = 99;
        EXPECT_FALSE(parser.validate(3, size, wide, 0xdeadbeef, 5, 7, end));
        EXPECT_EQ(end, 99u);
      }
    }
}
TEST(ZipDescriptorValidationTest, ReadFailuresCancellationChangedExtentAndBoundsFailClosed) {
  std::array<uint8_t, 24> scratch{};
  Source source;
  source.descriptor(true, true, 2, 5, 7);
  ZipDescriptorValidation parser(source, scratch);
  uint64_t end = 99;
  for (unsigned fail : {1u, 2u}) {
    source.reads = 0;
    source.failRead = fail;
    EXPECT_FALSE(parser.validate(3, source.bytes.size(), true, 2, 5, 7, end));
    EXPECT_EQ(end, 99u);
  }
  source.failRead = 0;
  source.changed = true;
  source.sizes = 0;
  EXPECT_FALSE(parser.validate(3, source.bytes.size(), true, 2, 5, 7, end));
  source.changed = false;
  ZipDescriptorValidation cancelled(source, scratch, [](void*) { return false; });
  EXPECT_FALSE(cancelled.validate(3, source.bytes.size(), true, 2, 5, 7, end));
  EXPECT_FALSE(parser.validate(UINT64_MAX, source.bytes.size(), true, 2, 5, 7, end));
  EXPECT_FALSE(parser.validate(3, source.bytes.size() + 1, true, 2, 5, 7, end));
  EXPECT_FALSE(parser.validate(3, source.bytes.size(), false, 2, UINT64_MAX, 7, end));
  EXPECT_EQ(end, 99u);
}
TEST(ZipDescriptorValidationTest, AmbiguousOwnershipIsRejected) {
  std::array<uint8_t, 24> scratch{};
  Source source;
  source.bytes.resize(16);
  for (size_t at = 0; at < 16; at += 4) inventory_detail::write(source.bytes, at, 0x08074b50, 4);
  ZipDescriptorValidation parser(source, scratch);
  uint64_t end = 99;
  EXPECT_FALSE(parser.validate(0, 16, false, 0x08074b50, 0x08074b50, 0x08074b50, end));
  EXPECT_EQ(end, 99u);
}
}  // namespace

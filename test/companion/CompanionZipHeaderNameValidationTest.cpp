#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "lib/Companion/CompanionZipHeaderNameValidation.h"
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
TEST(ZipHeaderNameTest, Utf8AndCp437ValidateAcrossEveryBankWidth) {
  std::array<uint8_t, 64> scratch{};
  for (bool utf8 : {false, true}) {
    Source source;
    source.bytes = utf8 ? std::vector<uint8_t>{'c', 'a', 'f', 0xc3, 0xa9, '.', 'i', 'f', 'o'}
                        : std::vector<uint8_t>{'c', 'a', 'f', 0x82, '.', 'i', 'f', 'o'};
    for (size_t width = 4; width <= 64; ++width) {
      ZipHeaderNameValidation parser(source, std::span(scratch).first(width));
      ZipPathDetails out;
      ASSERT_TRUE(parser.validate(0, source.bytes.size(), utf8, false, out)) << width;
      EXPECT_EQ(out.rawBytes, 9u);
    }
  }
}
TEST(ZipHeaderNameTest, DecodedLimitAndUnsafePathAreRejectedWithoutOutput) {
  std::array<uint8_t, 64> scratch{};
  Source source;
  source.bytes.assign(342, 0xb3);
  ZipHeaderNameValidation parser(source, scratch);
  ZipPathDetails out{99, 98, true};
  const auto saved = out;
  EXPECT_FALSE(parser.validate(0, source.bytes.size(), false, false, out));
  EXPECT_EQ(out, saved);
  source.bytes.assign(341, 0xb3);
  EXPECT_TRUE(parser.validate(0, source.bytes.size(), false, false, out));
  EXPECT_EQ(out.rawBytes, 1023u);
  for (auto bytes : {std::vector<uint8_t>{'.', '.', '/', 'a'}, {'/', 'a'}, {'a', 0}, {'a', 0xc0, 0xaf}}) {
    source.bytes = bytes;
    out = saved;
    EXPECT_FALSE(parser.validate(0, bytes.size(), true, false, out));
    EXPECT_EQ(out, saved);
  }
}
TEST(ZipHeaderNameTest, DirectorySlashAndEveryReadFailureCancellationRetry) {
  std::array<uint8_t, 4> scratch{};
  Source source;
  source.bytes = {'a', '/', 'b', '/'};
  ZipHeaderNameValidation parser(source, scratch);
  ZipPathDetails out;
  ASSERT_TRUE(parser.validate(0, 4, false, true, out));
  EXPECT_EQ(out.trimmedBytes, 3u);
  const auto reads = source.reads;
  EXPECT_FALSE(parser.validate(0, 4, false, false, out));
  for (unsigned fail = 1; fail <= reads; ++fail) {
    source.reads = 0;
    source.failRead = fail;
    out.rawBytes = 99;
    EXPECT_FALSE(parser.validate(0, 4, false, true, out));
    EXPECT_EQ(out.rawBytes, 99u);
  }
  source.failRead = 0;
  ZipHeaderNameValidation cancelled(source, scratch, [](void*) { return false; });
  EXPECT_FALSE(cancelled.validate(0, 4, false, true, out));
  EXPECT_TRUE(parser.validate(0, 4, false, true, out));
}
}  // namespace

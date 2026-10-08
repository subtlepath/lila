#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionDictzipValidation.h"

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
class CompanionDictzipValidation : public testing::Test {
 protected:
  Source source;
  tinfl_decompressor decoder{};
  std::array<uint8_t, 32768> window;
  std::array<uint8_t, 512> scratch;
  DictzipValidation validator{source, decoder, window, scratch};
  void SetUp() override {
    std::ifstream file(DICTZIP_FIXTURE, std::ios::binary);
    source.bytes = {std::istreambuf_iterator<char>(file), {}};
    ASSERT_GT(source.bytes.size(), 30u);
  }
};
TEST_F(CompanionDictzipValidation, ValidatesEveryInputBankWidth) {
  for (unsigned width = 12; width <= 512; ++width) {
    DictzipValidation sized(source, decoder, window, std::span(scratch).first(width));
    DictzipLayout result;
    ASSERT_TRUE(sized.validate(result)) << width;
    EXPECT_EQ(result.expandedBytes, 120000u);
  }
}
TEST_F(CompanionDictzipValidation, RejectsCorruptOutputAndTerminationWithoutChangingResult) {
  const auto original = source.bytes;
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    source.bytes = original;
    if (mutation == 0) source.bytes[source.bytes.size() - 8] ^= 1;
    if (mutation == 1) source.bytes[source.bytes.size() - 10] = 7;
    if (mutation == 2) source.bytes.insert(source.bytes.end() - 8, 0);
    if (mutation == 3) source.bytes[35] ^= 8;
    DictzipLayout result;
    result.expandedBytes = 99;
    EXPECT_FALSE(validator.validate(result)) << mutation;
    EXPECT_EQ(result.expandedBytes, 99u);
  }
}
TEST_F(CompanionDictzipValidation, ChecksOptionalHeaderCrc) {
  const auto original = source.bytes;
  source.bytes[3] |= 2;
  const size_t position = 12 + (source.bytes[10] | size_t(source.bytes[11]) << 8);
  const auto crc = inventoryIndexCrc(std::span(source.bytes).first(position));
  source.bytes.insert(source.bytes.begin() + position, {uint8_t(crc), uint8_t(crc >> 8)});
  DictzipLayout result;
  ASSERT_TRUE(validator.validate(result));
  source.bytes[position] ^= 1;
  EXPECT_FALSE(validator.validate(result));
  source.bytes = original;
  EXPECT_TRUE(validator.validate(result));
}
TEST_F(CompanionDictzipValidation, EveryReadFailureCanRetryAndCancellationPreservesOutput) {
  DictzipLayout result;
  ASSERT_TRUE(validator.validate(result));
  const auto count = source.reads;
  for (unsigned failure = 1; failure <= count; ++failure) {
    source.reads = 0;
    source.failRead = failure;
    result.expandedBytes = 99;
    ASSERT_FALSE(validator.validate(result)) << failure;
    EXPECT_EQ(result.expandedBytes, 99u);
  }
  source.failRead = 0;
  ASSERT_TRUE(validator.validate(result));
  DictzipValidation cancelled(source, decoder, window, scratch, [](void*) { return false; });
  result.expandedBytes = 99;
  EXPECT_FALSE(cancelled.validate(result));
  EXPECT_EQ(result.expandedBytes, 99u);
}
TEST_F(CompanionDictzipValidation, DecoderRejectsUnfilledRingReference) {
  static constexpr uint8_t compressed[] = {3, 2, 0};
  window.fill('A');
  tinfl_init(&decoder);
  size_t inBytes = sizeof(compressed), outBytes = window.size();
  EXPECT_LT(tinfl_decompress(&decoder, compressed, &inBytes, window.data(), window.data(), &outBytes,
                             TINFL_FLAG_VALIDATE_RING_HISTORY),
            TINFL_STATUS_DONE);
  EXPECT_EQ(outBytes, 0u);
}
TEST_F(CompanionDictzipValidation, RejectsGzipValidChunksThatShareHistory) {
  // zlib Z_SYNC_FLUSH preserves history; generic gzip decodes 128 A bytes.
  // The second RA chunk must instead be independently decompressible.
  source.bytes = {31,  139, 8, 4, 0,  0, 0,   0,   2,   255, 14,  0,   82, 65, 10,  0, 1,   0,
                  64,  0,   2, 0, 10, 0, 8,   0,   114, 116, 164, 12,  0,  0,  0,   0, 255, 255,
                  162, 20,  0, 0, 0,  0, 255, 255, 3,   0,   222, 138, 24, 4,  128, 0, 0,   0};
  DictzipLayout result;
  DictzipLayoutValidation structural(source, scratch);
  ASSERT_TRUE(structural.validate(result));
  window.fill('A');
  EXPECT_FALSE(validator.validate(result));
}
TEST_F(CompanionDictzipValidation, RejectsIncorrectIndependentChunkOutputLengths) {
  source.bytes[18] = uint8_t(50000);
  source.bytes[19] = uint8_t(50000 >> 8);
  DictzipLayout result;
  DictzipLayoutValidation structural(source, scratch);
  ASSERT_TRUE(structural.validate(result));
  EXPECT_FALSE(validator.validate(result));
}
}  // namespace

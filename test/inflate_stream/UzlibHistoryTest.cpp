#include <gtest/gtest.h>
#include <uzlib.h>

#include <array>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace {
class UzlibHistory : public testing::Test {
 protected:
  TINF_DATA decoder{};
  std::array<unsigned char, 8> ring{};
  std::array<unsigned char, 64> output{};

  void start(std::span<const unsigned char> input) {
    uzlib_uncompress_init(&decoder, ring.data(), ring.size());
    decoder.source = input.data();
    decoder.source_limit = input.data() + input.size();
    decoder.dest_start = output.data();
    decoder.dest = output.data();
    decoder.dest_limit = output.data() + output.size();
  }
};

TEST_F(UzlibHistory, RejectsReferenceBeforeFirstLiteral) {
  // Final fixed-Huffman block: length 3, distance 1, end of block.
  static constexpr unsigned char compressed[] = {0x03, 0x02, 0x00};
  ring.fill('A');
  start(compressed);
  EXPECT_EQ(uzlib_uncompress(&decoder), TINF_DATA_ERROR);
  EXPECT_EQ(decoder.dest, output.data());
  EXPECT_EQ(decoder.dict_filled, 0u);
}

TEST_F(UzlibHistory, AcceptsOverlappingReferenceAcrossOutputCalls) {
  // Literal A, length 3, distance 1: AAAA.
  static constexpr unsigned char compressed[] = {0x73, 0x04, 0x02, 0x00};
  start(compressed);
  decoder.dest_limit = output.data() + 1;
  ASSERT_EQ(uzlib_uncompress(&decoder), TINF_OK);
  EXPECT_EQ(decoder.dict_filled, 1u);
  decoder.dest_limit = output.data() + output.size();
  ASSERT_EQ(uzlib_uncompress(&decoder), TINF_DONE);
  EXPECT_EQ(decoder.dest - output.data(), 4);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(output.data()), 4), "AAAA");
  EXPECT_EQ(decoder.dict_filled, 4u);
}

TEST_F(UzlibHistory, SaturatesHistoryAndClearsItOnReset) {
  // Final stored block containing 12 literals; the eight-byte ring wraps.
  static constexpr unsigned char stored[] = {1,   12,  0,   0xf3, 0xff, 'a', 'b', 'c', 'd',
                                             'e', 'f', 'g', 'h',  'i',  'j', 'k', 'l'};
  start(stored);
  ASSERT_EQ(uzlib_uncompress(&decoder), TINF_DONE);
  EXPECT_EQ(decoder.dict_filled, ring.size());
  EXPECT_EQ(decoder.dict_idx, 4u);
  static constexpr unsigned char invalid[] = {0x03, 0x02, 0x00};
  start(invalid);
  EXPECT_EQ(decoder.dict_filled, 0u);
  EXPECT_EQ(uzlib_uncompress(&decoder), TINF_DATA_ERROR);
}

TEST_F(UzlibHistory, DecodesSharedDictzipAcrossHistoryWraps) {
  std::ifstream file(COMPANION_FIXTURE_DIR "/dictzip-chunks.dict.dz", std::ios::binary);
  ASSERT_TRUE(file.is_open());
  const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
  ASSERT_GT(bytes.size(), 28u);
  const size_t extraLength = bytes[10] | (static_cast<size_t>(bytes[11]) << 8);
  const size_t offset = 12 + extraLength;
  ASSERT_LT(offset, bytes.size() - 8);
  std::vector<unsigned char> history(32768);
  uzlib_uncompress_init(&decoder, history.data(), history.size());
  decoder.source = bytes.data() + offset;
  decoder.source_limit = bytes.data() + bytes.size() - 8;
  size_t expanded = 0;
  int status;
  do {
    decoder.dest = output.data();
    decoder.dest_limit = output.data() + output.size();
    status = uzlib_uncompress(&decoder);
    ASSERT_GE(status, 0);
    for (size_t index = 0; index < static_cast<size_t>(decoder.dest - output.data()); ++index) {
      ASSERT_EQ(output[index], static_cast<unsigned char>("abcdef"[(expanded + index) % 6]));
    }
    expanded += decoder.dest - output.data();
    ASSERT_LE(expanded, 120000u);
  } while (status != TINF_DONE);
  EXPECT_EQ(expanded, 120000u);
  EXPECT_EQ(decoder.dict_filled, history.size());
  EXPECT_EQ(decoder.source, decoder.source_limit);
}
}  // namespace

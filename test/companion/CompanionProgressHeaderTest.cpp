#include <gtest/gtest.h>

#include <array>

#include "core/srs/ProgressHeader.h"

using namespace tinta::core;
namespace {
std::array<uint8_t, 80> header() {
  std::array<uint8_t, 80> bytes{};
  std::memcpy(bytes.data(), "TIS1", 4);
  putU16(bytes.data() + 4, 1);
  putU16(bytes.data() + 6, 80);
  putU32(bytes.data() + 8, 17);
  putU32(bytes.data() + 12, 3);
  putU32(bytes.data() + 16, 42);
  putU16(bytes.data() + 26, 3);
  putU32(bytes.data() + 28, 1);
  ItemState::fresh(101).encode(bytes.data() + 32);
  putU32(bytes.data() + 48, 2);
  ItemState::fresh(202).encode(bytes.data() + 52);
  putU32(bytes.data() + 76, crc32(bytes.data(), 76));
  return bytes;
}
void checksum(std::array<uint8_t, 80>& bytes) { putU32(bytes.data() + 76, crc32(bytes.data(), 76)); }
}  // namespace

TEST(CompanionProgressHeader, PreservesPendingAndUndoEvidence) {
  const auto bytes = header();
  ProgressHeader result;
  ASSERT_TRUE(ProgressHeader::decode(bytes.data(), result));
  EXPECT_EQ(result.seq, 17u);
  EXPECT_EQ(result.recordCount, 3u);
  EXPECT_EQ(result.journalCount, 42u);
  EXPECT_TRUE(result.pendingValid);
  EXPECT_EQ(result.pendingSlot, 1u);
  EXPECT_EQ(result.pending.uid, 101u);
  EXPECT_TRUE(result.undoValid);
  EXPECT_EQ(result.undoSlot, 2u);
  EXPECT_EQ(result.undoBefore.uid, 202u);
}

TEST(CompanionProgressHeader, EverySingleBitCorruptionRefusesWithoutChangingOutput) {
  const auto original = header();
  for (size_t bit = 0; bit < original.size() * 8; ++bit) {
    auto bytes = original;
    bytes[bit / 8] ^= uint8_t(1u << (bit % 8));
    ProgressHeader result;
    result.seq = 999;
    result.pending.uid = 888;
    EXPECT_FALSE(ProgressHeader::decode(bytes.data(), result)) << bit;
    EXPECT_EQ(result.seq, 999u);
    EXPECT_EQ(result.pending.uid, 888u);
  }
}

TEST(CompanionProgressHeader, RejectsOutOfRangeSlotsAndMalformedBeforeImages) {
  for (unsigned fault = 0; fault < 5; ++fault) {
    auto bytes = header();
    if (fault == 0) putU32(bytes.data() + 12, 32768);
    if (fault == 1) putU32(bytes.data() + 28, 3);
    if (fault == 2) putU32(bytes.data() + 48, 3);
    if (fault == 3) bytes[32 + 11] = 0x80;
    if (fault == 4) bytes[52 + 11] = 0x80;
    checksum(bytes);
    ProgressHeader result;
    EXPECT_FALSE(ProgressHeader::decode(bytes.data(), result)) << fault;
  }
}

TEST(CompanionProgressHeader, IgnoresInactiveBeforeImagesAsLocalRecoveryDoes) {
  auto bytes = header();
  putU16(bytes.data() + 26, 0);
  putU32(bytes.data() + 28, UINT32_MAX);
  putU32(bytes.data() + 48, UINT32_MAX);
  bytes[43] = bytes[63] = 0x80;
  checksum(bytes);
  ProgressHeader result;
  ASSERT_TRUE(ProgressHeader::decode(bytes.data(), result));
  EXPECT_FALSE(result.pendingValid);
  EXPECT_FALSE(result.undoValid);
}

#include <gtest/gtest.h>

#include "lib/Companion/CompanionRemovalMetadataSnapshot.h"
using namespace companion;
namespace {
RemovalMetadataSnapshot snapshot() {
  RemovalMetadataSnapshot value;
  value.request.transaction.fill(1);
  value.request.owner.fill(2);
  value.request.generation.fill(3);
  value.request.manifest.kind = ContentKind::Epub;
  value.request.manifest.formatVersion = 1;
  value.request.manifest.length = 123;
  value.request.manifest.contentHash.fill(4);
  value.planHash.fill(5);
  value.previousHash.fill(6);
  value.nextHash.fill(7);
  value.previousLength = 1234;
  value.nextLength = 5678;
  return value;
}
}  // namespace
TEST(RemovalMetadataSnapshot, RoundtripAndUnalignedBufferPreserveBothGenerations) {
  for (const auto file : {RemovalMetadataFile::State, RemovalMetadataFile::Recent}) {
    auto value = snapshot();
    value.file = file;
    std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE + 2> buffer{};
    buffer.front() = buffer.back() = 0xA5;
    auto bytes = std::span(buffer).subspan(1, REMOVAL_METADATA_SNAPSHOT_SIZE);
    ASSERT_EQ(encodeRemovalMetadataSnapshot(value, bytes), bytes.size());
    RemovalMetadataSnapshot result;
    ASSERT_TRUE(decodeRemovalMetadataSnapshot(bytes, result));
    EXPECT_EQ(result, value);
    EXPECT_EQ(buffer.front(), 0xA5);
    EXPECT_EQ(buffer.back(), 0xA5);
    EXPECT_EQ(bytes[219], 0xD2);
    EXPECT_EQ(bytes[220], 0x04);
  }
}
TEST(RemovalMetadataSnapshot, CorruptionTruncationAndUnknownVersionsPreserveOutput) {
  const auto value = snapshot();
  std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> original{};
  ASSERT_EQ(encodeRemovalMetadataSnapshot(value, original), original.size());
  auto output = value;
  output.nextHash.fill(9);
  const auto sentinel = output;
  for (size_t at = 0; at < original.size(); ++at) {
    auto bytes = original;
    bytes[at] ^= 1;
    EXPECT_FALSE(decodeRemovalMetadataSnapshot(bytes, output));
    EXPECT_EQ(output, sentinel);
    EXPECT_FALSE(decodeRemovalMetadataSnapshot(std::span(original).first(at), output));
    EXPECT_EQ(output, sentinel);
  }
}
TEST(RemovalMetadataSnapshot, AbsentOriginalIsExplicitAndInvalidDeclarationsDoNotWrite) {
  auto value = snapshot();
  value.previousLength = 0;
  value.previousHash = {};
  std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> bytes{};
  EXPECT_EQ(encodeRemovalMetadataSnapshot(value, bytes), bytes.size());
  for (unsigned fault = 0; fault < 9; ++fault) {
    auto invalid = value;
    switch (fault) {
      case 0:
        invalid.previousHash.fill(6);
        break;
      case 1:
        invalid.previousLength = 1;
        break;
      case 2:
        invalid.nextLength = 0;
        break;
      case 3:
        invalid.nextLength = REMOVAL_METADATA_MAX_BYTES + 1;
        break;
      case 4:
        invalid.nextHash = {};
        break;
      case 5:
        invalid.planHash = {};
        break;
      case 6:
        invalid.request.owner = {};
        break;
      case 7:
        invalid.file = static_cast<RemovalMetadataFile>(2);
        break;
      case 8:
        invalid.previousLength = REMOVAL_METADATA_MAX_BYTES + 1;
        invalid.previousHash.fill(6);
        break;
    }
    bytes.fill(0xA5);
    EXPECT_EQ(encodeRemovalMetadataSnapshot(invalid, bytes), 0);
    EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](auto byte) { return byte == 0xA5; }));
  }
}
TEST(RemovalMetadataSnapshot, ValidCrcDoesNotAdmitInvalidOwnershipOrGenerationSemantics) {
  std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> original{};
  ASSERT_EQ(encodeRemovalMetadataSnapshot(snapshot(), original), original.size());
  for (const auto offset : {size_t{28}, size_t{44}, size_t{123}, size_t{187}}) {
    auto bytes = original;
    const size_t length = offset < 100 ? 16 : 32;
    std::fill_n(bytes.begin() + offset, length, 0);
    inventory_detail::write(bytes, 236, inventoryIndexCrc(std::span(bytes).first(236)), 4);
    RemovalMetadataSnapshot output;
    EXPECT_FALSE(decodeRemovalMetadataSnapshot(bytes, output));
  }
}

TEST(RemovalMetadataSnapshot, UnchangedExistingMetadataRetainsItsOriginalBytes) {
  auto value = snapshot();
  value.nextLength = value.previousLength;
  value.nextHash = value.previousHash;
  EXPECT_TRUE(validRemovalMetadataSnapshot(value));
  EXPECT_TRUE(unchangedRemovalMetadataSnapshot(value));
  std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> bytes{};
  ASSERT_EQ(encodeRemovalMetadataSnapshot(value, bytes), bytes.size());
  RemovalMetadataSnapshot result;
  ASSERT_TRUE(decodeRemovalMetadataSnapshot(bytes, result));
  EXPECT_EQ(result, value);
}

TEST(RemovalMetadataSnapshot, OverlappingInputPreservesObjectBytes) {
  struct Arena {
    std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> prefix{};
    RemovalMetadataSnapshot output;
    std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> suffix{};
  } arena;
  std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> encoded{};
  ASSERT_EQ(encodeRemovalMetadataSnapshot(snapshot(), encoded), encoded.size());
  auto bytes = std::span<uint8_t>(reinterpret_cast<uint8_t*>(&arena), sizeof(arena));
  const auto start = reinterpret_cast<uint8_t*>(&arena.output) - bytes.data();
  const auto end = start + sizeof(arena.output);
  for (size_t at = start - encoded.size() + 1; at < end; ++at) {
    arena.output = snapshot();
    std::copy(encoded.begin(), encoded.end(), bytes.begin() + at);
    std::array<uint8_t, sizeof(Arena)> sentinel{};
    std::copy(bytes.begin(), bytes.end(), sentinel.begin());
    EXPECT_FALSE(decodeRemovalMetadataSnapshot(bytes.subspan(at, encoded.size()), arena.output));
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), sentinel.begin()));
  }
}

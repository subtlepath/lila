#include <gtest/gtest.h>

#include "../../lib/Companion/CompanionInventoryPairValidation.h"
using namespace companion;
namespace {
struct Storage : InventoryIndexStorage {
  std::vector<uint8_t> bytes;
  unsigned reads = 0, failAt = 0;
  bool size(uint64_t& size) override {
    size = bytes.size();
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (++reads == failAt || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
Identity generation() {
  Identity value{};
  value[0] = 1;
  return value;
}
ContentManifest item(unsigned value) {
  ContentManifest manifest;
  manifest.contentHash[0] = value;
  manifest.length = value + 1;
  return manifest;
}
Storage index(unsigned count) {
  Storage result;
  result.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + count * INVENTORY_INDEX_ENTRY_SIZE);
  for (unsigned i = 0; i < count; ++i)
    encodeInventoryIndexEntry(
        item(i), std::span(result.bytes).subspan(INVENTORY_INDEX_HEADER_SIZE + i * INVENTORY_INDEX_ENTRY_SIZE));
  InventoryIndexHeader header{generation(), 9, count,
                              inventoryIndexCrc(std::span(result.bytes).subspan(INVENTORY_INDEX_HEADER_SIZE))};
  encodeInventoryIndexHeader(header, result.bytes);
  return result;
}
Storage paths(const std::vector<ContentManifest>& entries) {
  Storage result;
  result.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + entries.size() * INVENTORY_PATH_MAX_RECORD);
  size_t offset = INVENTORY_INDEX_HEADER_SIZE;
  for (const auto& entry : entries)
    offset += encodeInventoryPath(entry, "/books/book.epub", std::span(result.bytes).subspan(offset));
  result.bytes.resize(offset);
  InventoryIndexHeader header{generation(), 9, entries.size(),
                              inventoryIndexCrc(std::span(result.bytes).subspan(INVENTORY_INDEX_HEADER_SIZE))};
  encodeInventoryPathsHeader(header, result.bytes);
  return result;
}
std::vector<ContentManifest> entries(unsigned count) {
  std::vector<ContentManifest> result;
  result.reserve(count);
  for (unsigned i = count; i > 0; --i) result.push_back(item(i - 1));
  return result;
}
}  // namespace
TEST(CompanionInventoryPairValidation, UnsortedDuplicatePathsAndMultipleCoveragePasses) {
  auto catalog = index(19);
  auto records = entries(19);
  records.push_back(item(3));
  auto map = paths(records);
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD + 1> scratch;
  InventoryPairValidation validation(catalog, map, scratch);
  ASSERT_TRUE(validation.validate(generation(), 9));
  EXPECT_EQ(validation.revision(), 9u);
  EXPECT_TRUE(validation.validate(generation()));
  EXPECT_FALSE(validation.validate(generation(), 10));
  EXPECT_EQ(validation.revision(), 0u);
}
TEST(CompanionInventoryPairValidation, RejectsMissingExtraAndConflictingManifestsWithValidCrcs) {
  for (unsigned failure = 0; failure < 3; ++failure) {
    auto catalog = index(19);
    auto records = entries(19);
    if (failure == 0) records.pop_back();
    if (failure == 1) records.push_back(item(42));
    if (failure == 2) ++records[3].length;
    auto map = paths(records);
    std::array<uint8_t, 8192> scratch;
    InventoryPairValidation validation(catalog, map, scratch);
    EXPECT_FALSE(validation.validate(generation(), 9));
    EXPECT_EQ(validation.revision(), 0u);
  }
}
TEST(CompanionInventoryPairValidation, EmptySnapshotsAndInsufficientScratch) {
  auto catalog = index(0);
  auto map = paths({});
  std::array<uint8_t, 8192> scratch;
  InventoryPairValidation validation(catalog, map, scratch);
  EXPECT_TRUE(validation.validate(generation(), 9));
  InventoryPairValidation small(catalog, map, std::span(scratch).first(INVENTORY_PATH_MAX_RECORD));
  EXPECT_FALSE(small.validate(generation(), 9));
  auto extra = paths({item(0)});
  InventoryPairValidation invalid(catalog, extra, scratch);
  EXPECT_FALSE(invalid.validate(generation(), 9));
}
TEST(CompanionInventoryPairValidation, EveryReadFailureAndRecordCorruptionInvalidatesPair) {
  auto catalog = index(3);
  auto map = paths(entries(3));
  std::array<uint8_t, 8192> scratch;
  InventoryPairValidation success(catalog, map, scratch);
  ASSERT_TRUE(success.validate(generation(), 9));
  const unsigned indexReads = catalog.reads, mapReads = map.reads;
  for (unsigned side = 0; side < 2; ++side) {
    for (unsigned failure = 1; failure <= (side == 0 ? indexReads : mapReads); ++failure) {
      auto left = index(3);
      auto right = paths(entries(3));
      if (side == 0)
        left.failAt = failure;
      else
        right.failAt = failure;
      InventoryPairValidation validation(left, right, scratch);
      EXPECT_FALSE(validation.validate(generation(), 9));
      EXPECT_EQ(validation.revision(), 0u);
    }
  }
  map.bytes.back() ^= 1;
  InventoryPairValidation corrupt(catalog, map, scratch);
  EXPECT_FALSE(corrupt.validate(generation(), 9));
}

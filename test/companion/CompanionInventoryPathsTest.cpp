#include <gtest/gtest.h>

#include "../../lib/Companion/CompanionInventoryPaths.h"
using namespace companion;
namespace {
struct Storage : InventoryIndexStorage {
  std::vector<uint8_t> bytes;
  bool fail = false;
  bool size(uint64_t& size) override {
    size = bytes.size();
    return !fail;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (fail || offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
Identity generation() {
  Identity result{};
  result[0] = 1;
  return result;
}
ContentManifest manifest() {
  ContentManifest result;
  result.contentHash[0] = 3;
  result.length = 42;
  return result;
}
Storage snapshot() {
  Storage storage;
  storage.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + 2 * INVENTORY_PATH_MAX_RECORD);
  size_t size = INVENTORY_INDEX_HEADER_SIZE;
  for (const char* path : {"/books/renamed.epub", "/elsewhere/same.epub"}) {
    size += encodeInventoryPath(manifest(), path, std::span(storage.bytes).subspan(size));
  }
  storage.bytes.resize(size);
  InventoryIndexHeader header{generation(), 7, 2,
                              inventoryIndexCrc(std::span(storage.bytes).subspan(INVENTORY_INDEX_HEADER_SIZE))};
  EXPECT_EQ(encodeInventoryPathsHeader(header, storage.bytes), INVENTORY_INDEX_HEADER_SIZE);
  return storage;
}
}  // namespace
TEST(CompanionInventoryPaths, ValidatesPathsAndRoundTripsMaximumUtf8Path) {
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> scratch;
  const std::string path = "/" + std::string(508, 'a') + "é";
  ASSERT_EQ(path.size(), INVENTORY_PATH_LIMIT);
  const auto size = encodeInventoryPath(manifest(), path, scratch);
  ASSERT_EQ(size, scratch.size());
  InventoryPathRecord decoded;
  ASSERT_TRUE(decodeInventoryPath(scratch, decoded));
  EXPECT_EQ(decoded.manifest, manifest());
  EXPECT_EQ(decoded.path, path);
  for (const char* invalid : {"", "/", "relative", "/../secret", "/a/./b", "/a//b", "/a/", "/a\\b", "/a:b", "/a\nb"}) {
    EXPECT_EQ(encodeInventoryPath(manifest(), invalid, scratch), 0u);
  }
  EXPECT_EQ(encodeInventoryPath(manifest(), "/" + std::string(511, 'a'), scratch), 0u);
}
TEST(CompanionInventoryPaths, RejectsEveryTruncationCorruptionAndTrailingBytes) {
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> scratch;
  const size_t size = encodeInventoryPath(manifest(), "/book.epub", scratch);
  InventoryPathRecord record;
  record.path = "unchanged";
  for (size_t length = 0; length < size; ++length)
    EXPECT_FALSE(decodeInventoryPath(std::span(scratch).first(length), record));
  EXPECT_EQ(record.path, "unchanged");
  for (size_t byte = 0; byte < size; ++byte) {
    scratch[byte] ^= 1;
    EXPECT_FALSE(decodeInventoryPath(std::span(scratch).first(size), record));
    scratch[byte] ^= 1;
  }
  EXPECT_FALSE(decodeInventoryPath(std::span(scratch).first(size + 1), record));
}
TEST(CompanionInventoryPaths, RequiresMatchingSnapshotAndCopiesTerminatedPath) {
  auto storage = snapshot();
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> scratch;
  InventoryPaths paths(storage, scratch);
  auto wrongGeneration = generation();
  wrongGeneration[1] = 1;
  EXPECT_FALSE(paths.open(wrongGeneration, 7));
  EXPECT_FALSE(paths.open(generation(), 8));
  ASSERT_TRUE(paths.open(generation(), 7));
  std::array<char, 512> path;
  path.fill('x');
  ASSERT_EQ(paths.find(manifest(), path), InventoryPathResult::Found);
  EXPECT_STREQ(path.data(), "/books/renamed.epub");
  auto missing = manifest();
  missing.contentHash[0] = 4;
  EXPECT_EQ(paths.find(missing, path), InventoryPathResult::Missing);
  auto mismatch = manifest();
  ++mismatch.length;
  EXPECT_EQ(paths.find(mismatch, path), InventoryPathResult::Error);
  EXPECT_STREQ(path.data(), "/books/renamed.epub");
  EXPECT_EQ(paths.find(manifest(), std::span(path).first(2)), InventoryPathResult::Error);
}
TEST(CompanionInventoryPaths, CompleteValidationAndLaterReadFailuresFailClosed) {
  auto original = snapshot();
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> scratch;
  for (size_t byte = 0; byte < original.bytes.size(); ++byte) {
    auto storage = original;
    storage.bytes[byte] ^= 1;
    InventoryPaths paths(storage, scratch);
    EXPECT_FALSE(paths.open(generation(), 7));
  }
  auto storage = original;
  InventoryPaths paths(storage, scratch);
  ASSERT_TRUE(paths.open(generation(), 7));
  storage.fail = true;
  std::array<char, 512> path;
  EXPECT_EQ(paths.find(manifest(), path), InventoryPathResult::Error);
  storage.fail = false;
  EXPECT_EQ(paths.find(manifest(), path), InventoryPathResult::Error);
}

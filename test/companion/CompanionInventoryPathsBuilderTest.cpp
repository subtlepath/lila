#include <gtest/gtest.h>

#include "../../lib/Companion/CompanionInventoryPathsBuilder.h"
using namespace companion;
namespace {
struct Stage : InventoryPathsStage, InventoryIndexStorage {
  std::vector<uint8_t> candidate;
  unsigned operations = 0, failAt = 0, aborts = 0, begins = 0;
  bool sealed = false;
  Stage() { candidate.reserve(2048); }
  bool fail() { return ++operations == failAt; }
  bool begin() override {
    ++begins;
    sealed = false;
    candidate.clear();
    return !fail();
  }
  bool write(uint64_t offset, std::span<const uint8_t> bytes) override {
    if (fail()) return false;
    if (offset + bytes.size() > candidate.size()) candidate.resize(offset + bytes.size());
    std::copy(bytes.begin(), bytes.end(), candidate.begin() + offset);
    return true;
  }
  bool seal(uint64_t size) override {
    if (fail()) return false;
    candidate.resize(size);
    sealed = true;
    return true;
  }
  void abort() override {
    ++aborts;
    candidate.clear();
    sealed = false;
  }
  bool size(uint64_t& bytes) override {
    bytes = candidate.size();
    return sealed;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (!sealed || offset > candidate.size() || output.size() > candidate.size() - offset) return false;
    std::copy_n(candidate.begin() + offset, output.size(), output.begin());
    return true;
  }
};
Identity generation() {
  Identity value{};
  value[0] = 1;
  return value;
}
ContentManifest item(unsigned hash) {
  ContentManifest value;
  value.contentHash[0] = hash;
  value.length = 9;
  return value;
}
bool build(Stage& stage, std::span<uint8_t> scratch) {
  InventoryPathsBuilder builder(stage, scratch);
  return builder.begin(generation(), 4) && builder.record(item(1), "/books/a.epub") &&
         builder.record(item(1), "/renamed/a.epub") && builder.record(item(2), "/books/b.epub") && builder.seal();
}
}  // namespace
TEST(CompanionInventoryPathsBuilder, SealsCompleteMapWithDuplicateLocationsAndMatchingRevision) {
  Stage stage;
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> scratch;
  ASSERT_TRUE(build(stage, scratch));
  EXPECT_TRUE(stage.sealed);
  EXPECT_EQ(stage.aborts, 0u);
  InventoryPaths paths(stage, scratch);
  ASSERT_TRUE(paths.open(generation(), 4));
  std::array<char, 512> output;
  ASSERT_EQ(paths.find(item(1), output), InventoryPathResult::Found);
  EXPECT_STREQ(output.data(), "/books/a.epub");
  ASSERT_EQ(paths.find(item(2), output), InventoryPathResult::Found);
  EXPECT_STREQ(output.data(), "/books/b.epub");
}
TEST(CompanionInventoryPathsBuilder, EmptyMapIsValidOnlyAfterSeal) {
  Stage stage;
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> scratch;
  InventoryPathsBuilder builder(stage, scratch);
  ASSERT_TRUE(builder.begin(generation(), 4));
  InventoryPaths paths(stage, scratch);
  EXPECT_FALSE(paths.open(generation(), 4));
  ASSERT_TRUE(builder.seal());
  ASSERT_TRUE(paths.open(generation(), 4));
  std::array<char, 8> output;
  EXPECT_EQ(paths.find(item(1), output), InventoryPathResult::Missing);
}
TEST(CompanionInventoryPathsBuilder, EveryMutationFailureDiscardsCandidate) {
  Stage success;
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> scratch;
  ASSERT_TRUE(build(success, scratch));
  for (unsigned failure = 1; failure <= success.operations; ++failure) {
    Stage stage;
    stage.failAt = failure;
    EXPECT_FALSE(build(stage, scratch));
    EXPECT_EQ(stage.aborts, 1u);
    EXPECT_FALSE(stage.sealed);
    EXPECT_TRUE(stage.candidate.empty());
  }
}
TEST(CompanionInventoryPathsBuilder, InvalidInputAbortsAndCannotResumeWithoutBegin) {
  Stage stage;
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> scratch;
  InventoryPathsBuilder builder(stage, scratch);
  ASSERT_TRUE(builder.begin(generation(), 4));
  EXPECT_FALSE(builder.record(item(1), "/../private"));
  EXPECT_FALSE(builder.record(item(1), "/valid.epub"));
  EXPECT_FALSE(builder.seal());
  EXPECT_EQ(stage.aborts, 1u);
  ASSERT_TRUE(builder.begin(generation(), 5));
  ASSERT_TRUE(builder.record(item(1), "/valid.epub"));
  ASSERT_TRUE(builder.seal());
  InventoryPaths paths(stage, scratch);
  ASSERT_TRUE(paths.open(generation(), 5));
}
TEST(CompanionInventoryPathsBuilder, DestructorAbortsIncompleteScanAndInvalidArgumentsDoNotOpenStage) {
  Stage stage;
  std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> scratch;
  {
    InventoryPathsBuilder builder(stage, scratch);
    EXPECT_FALSE(builder.begin(Identity{}, 4));
    EXPECT_FALSE(builder.begin(generation(), 0));
    EXPECT_EQ(stage.begins, 0u);
    ASSERT_TRUE(builder.begin(generation(), 4));
    ASSERT_TRUE(builder.record(item(1), "/book.epub"));
  }
  EXPECT_EQ(stage.aborts, 1u);
  EXPECT_TRUE(stage.candidate.empty());
  InventoryPathsBuilder small(stage, std::span(scratch).first(100));
  EXPECT_FALSE(small.begin(generation(), 4));
  EXPECT_EQ(stage.begins, 1u);
}

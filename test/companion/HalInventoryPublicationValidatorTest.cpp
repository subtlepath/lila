#include <gtest/gtest.h>

#include "../../lib/Companion/CompanionInventoryRollback.h"
#include "../../lib/hal/HalInventoryPublicationValidator.h"
#include "../../lib/hal/HalInventoryRecovery.h"
#include "../../lib/hal/HalTransferStorage.h"
using namespace companion;
namespace {
Identity generation() {
  Identity value{};
  value[0] = 1;
  return value;
}
ContentManifest item(unsigned value) {
  ContentManifest manifest;
  manifest.contentHash[0] = value;
  manifest.length = 99;
  return manifest;
}
std::vector<uint8_t> snapshot(bool paths, bool mismatch = false) {
  std::vector<uint8_t> bytes(INVENTORY_INDEX_HEADER_SIZE + 2 * INVENTORY_PATH_MAX_RECORD);
  size_t offset = INVENTORY_INDEX_HEADER_SIZE;
  for (unsigned value = 1; value <= 2; ++value) {
    auto manifest = item(value);
    if (mismatch && value == 2) ++manifest.length;
    offset += paths ? encodeInventoryPath(manifest, "/books/book.epub", std::span(bytes).subspan(offset))
                    : encodeInventoryIndexEntry(manifest, std::span(bytes).subspan(offset));
  }
  bytes.resize(offset);
  InventoryIndexHeader header{generation(), 7, 2,
                              inventoryIndexCrc(std::span(bytes).subspan(INVENTORY_INDEX_HEADER_SIZE))};
  if (paths)
    encodeInventoryPathsHeader(header, bytes);
  else
    encodeInventoryIndexHeader(header, bytes);
  return bytes;
}
class HalPublicationValidationTest : public testing::Test {
 protected:
  void SetUp() override {
    inventory_hal_test::state = {};
    inventory_hal_test::state.files["/index"] = snapshot(false);
    inventory_hal_test::state.files["/paths"] = snapshot(true);
  }
};
}  // namespace
TEST_F(HalPublicationValidationTest, ValidatesBothFormatsAndCorrespondenceWithTwoClosedHandles) {
  std::array<uint8_t, 8192> scratch;
  HalInventoryPublicationValidator validator(scratch);
  uint64_t revision = 0;
  ASSERT_EQ(validator.pair("/index", "/paths", generation(), 0, &revision), InventoryValidation::Valid);
  EXPECT_EQ(revision, 7u);
  EXPECT_EQ(inventory_hal_test::state.closes, 2u);
  EXPECT_EQ(validator.file("/index", false, generation(), 7), InventoryValidation::Valid);
  EXPECT_EQ(validator.file("/paths", true, generation(), 0), InventoryValidation::Valid);
  EXPECT_EQ(inventory_hal_test::state.closes, 4u);
}
TEST_F(HalPublicationValidationTest, RejectsCrcValidMismatchAndWrongRevisionsAndMissingFiles) {
  std::array<uint8_t, 8192> scratch;
  HalInventoryPublicationValidator validator(scratch);
  uint64_t revision = 42;
  inventory_hal_test::state.files["/paths"] = snapshot(true, true);
  EXPECT_EQ(validator.pair("/index", "/paths", generation(), 7, &revision), InventoryValidation::Invalid);
  EXPECT_EQ(revision, 42u);
  EXPECT_EQ(validator.file("/index", false, generation(), 8), InventoryValidation::Invalid);
  EXPECT_EQ(validator.file("/paths", true, generation(), 8), InventoryValidation::Invalid);
  EXPECT_EQ(validator.pair("/index", "/missing", generation(), 7), InventoryValidation::Invalid);
  EXPECT_EQ(validator.file(nullptr, false, generation(), 7), InventoryValidation::Invalid);
  EXPECT_GT(inventory_hal_test::state.errors, 0u);
}
TEST_F(HalPublicationValidationTest, ReadOpenAndCloseErrorsRemainIoErrorsAndPreserveOutput) {
  for (unsigned failure = 0; failure < 3; ++failure) {
    auto& state = inventory_hal_test::state;
    state.statErrorPath.clear();
    state.readErrorPath.clear();
    state.failClose = false;
    std::array<uint8_t, 8192> scratch;
    HalInventoryPublicationValidator validator(scratch);
    if (failure == 0) state.statErrorPath = "/paths";
    if (failure == 1) state.readErrorPath = "/paths";
    if (failure == 2) state.failClose = true;
    uint64_t revision = 42;
    EXPECT_EQ(validator.pair("/index", "/paths", generation(), 7, &revision), InventoryValidation::IoError);
    EXPECT_EQ(revision, 42u);
    state.statErrorPath.clear();
    state.readErrorPath.clear();
    state.failClose = false;
    EXPECT_EQ(validator.pair("/index", "/paths", generation(), 7, &revision), InventoryValidation::Valid);
    EXPECT_EQ(revision, 7u);
  }
}
TEST_F(HalPublicationValidationTest, SnapshotCloseReportsFailureAndBlocksReopenInThatCall) {
  HalInventoryIndexStorage storage;
  ASSERT_TRUE(storage.open("/index"));
  inventory_hal_test::state.failClose = true;
  EXPECT_FALSE(storage.open("/paths"));
  inventory_hal_test::state.failClose = false;
  EXPECT_TRUE(storage.open("/paths"));
  inventory_hal_test::state.failClose = true;
  EXPECT_FALSE(storage.close());
}

namespace {
void preparePublication() {
  auto& files = inventory_hal_test::state.files;
  files[InventoryPublication::INDEX] = snapshot(false);
  files[InventoryPublication::PATHS] = snapshot(true);
  for (bool paths : {false, true}) {
    auto bytes = snapshot(paths);
    InventoryIndexHeader header;
    if (paths)
      decodeInventoryPathsHeader(std::span(bytes).first(INVENTORY_INDEX_HEADER_SIZE), header);
    else
      decodeInventoryIndexHeader(std::span(bytes).first(INVENTORY_INDEX_HEADER_SIZE), header);
    header.revision = 8;
    if (paths)
      encodeInventoryPathsHeader(header, bytes);
    else
      encodeInventoryIndexHeader(header, bytes);
    files[paths ? InventoryPublication::PATHS_NEXT : InventoryPublication::INDEX_NEXT] = bytes;
  }
}
}  // namespace
TEST_F(HalPublicationValidationTest, ActualHalPublicationResumesEachFailedRename) {
  for (unsigned failure = 1; failure <= 4; ++failure) {
    inventory_hal_test::state = {};
    preparePublication();
    std::array<uint8_t, 8192> scratch;
    HalTransferStorage storage;
    HalInventoryPublicationValidator validator(scratch);
    InventoryPublication publication(storage, validator, scratch);
    inventory_hal_test::state.failRename = failure;
    ASSERT_EQ(publication.publish(generation(), 8), InventoryPublicationResult::IoError);
    inventory_hal_test::state.failRename = 0;
    InventoryPublication restarted(storage, validator, scratch);
    ASSERT_EQ(restarted.recover(generation()), InventoryPublicationResult::Ok);
    ASSERT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), 8),
              InventoryValidation::Valid);
    EXPECT_EQ(restarted.publish(generation(), 8), InventoryPublicationResult::Ok);
    EXPECT_FALSE(inventory_hal_test::state.files.contains(InventoryPublication::INDEX_OLD));
    EXPECT_FALSE(inventory_hal_test::state.files.contains(InventoryPublication::PATHS_OLD));
  }
}
TEST_F(HalPublicationValidationTest, CloseFailurePreventsIntentAndRenamesThroughActualHal) {
  preparePublication();
  std::array<uint8_t, 8192> scratch;
  HalTransferStorage storage;
  HalInventoryPublicationValidator validator(scratch);
  InventoryPublication publication(storage, validator, scratch);
  inventory_hal_test::state.failClose = true;
  EXPECT_EQ(publication.publish(generation(), 8), InventoryPublicationResult::IoError);
  EXPECT_EQ(inventory_hal_test::state.renames, 0u);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(InventoryPublication::INTENTS[0]));
  inventory_hal_test::state.failClose = false;
}

TEST_F(HalPublicationValidationTest, CorruptCandidateRollsBackMixedPairAndResumesInterruptedRestore) {
  for (bool interrupted : {false, true}) {
    inventory_hal_test::state = {};
    preparePublication();
    std::array<uint8_t, 8192> scratch;
    HalTransferStorage storage;
    HalInventoryPublicationValidator validator(scratch);
    InventoryPublication publication(storage, validator, scratch);
    inventory_hal_test::state.failRename = 3;
    ASSERT_EQ(publication.publish(generation(), 8), InventoryPublicationResult::IoError);
    inventory_hal_test::state.failRename = 0;
    inventory_hal_test::state.files.at(InventoryPublication::PATHS_NEXT).back() ^= 1;
    EXPECT_EQ(publication.recover(generation()), InventoryPublicationResult::Corrupt);
    InventoryRollback rollback(storage, validator, scratch);
    bool pending = true;
    ASSERT_TRUE(rollback.pending(pending));
    EXPECT_FALSE(pending);
    if (interrupted) {
      inventory_hal_test::state.renames = 0;
      inventory_hal_test::state.failRename = 1;
      ASSERT_EQ(rollback.begin(generation(), 8), InventoryPublicationResult::IoError);
      ASSERT_TRUE(rollback.pending(pending));
      EXPECT_TRUE(pending);
      inventory_hal_test::state.failRename = 0;
      InventoryRollback restarted(storage, validator, scratch);
      ASSERT_EQ(restarted.recover(generation()), InventoryPublicationResult::Ok);
    } else
      ASSERT_EQ(rollback.begin(generation(), 8), InventoryPublicationResult::Ok);
    EXPECT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), 7),
              InventoryValidation::Valid);
    EXPECT_FALSE(inventory_hal_test::state.files.contains(InventoryPublication::INTENTS[0]));
    EXPECT_FALSE(inventory_hal_test::state.files.contains(InventoryRollback::INTENTS[0]));
    ASSERT_TRUE(rollback.pending(pending));
    EXPECT_FALSE(pending);
  }
}

TEST_F(HalPublicationValidationTest, HalRecoveryEntryPointRestoresOldPairOrEmptyBeforeReturning) {
  for (bool initial : {false, true}) {
    inventory_hal_test::state = {};
    preparePublication();
    if (initial) {
      inventory_hal_test::state.files.erase(InventoryPublication::INDEX);
      inventory_hal_test::state.files.erase(InventoryPublication::PATHS);
    }
    std::array<uint8_t, 8192> scratch;
    HalTransferStorage storage;
    HalInventoryPublicationValidator validator(scratch);
    InventoryPublication publication(storage, validator, scratch);
    inventory_hal_test::state.failRename = initial ? 2 : 3;
    ASSERT_EQ(publication.publish(generation(), 8), InventoryPublicationResult::IoError);
    inventory_hal_test::state.failRename = 0;
    inventory_hal_test::state.files.at(InventoryPublication::PATHS_NEXT).back() ^= 1;
    ASSERT_EQ(recoverInventorySnapshots(storage, generation(), scratch), InventoryPublicationResult::Ok);
    if (initial) {
      EXPECT_FALSE(inventory_hal_test::state.files.contains(InventoryPublication::INDEX));
      EXPECT_FALSE(inventory_hal_test::state.files.contains(InventoryPublication::PATHS));
    } else
      EXPECT_EQ(validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation(), 7),
                InventoryValidation::Valid);
    EXPECT_EQ(recoverInventorySnapshots(storage, generation(), scratch), InventoryPublicationResult::Ok);
  }
}

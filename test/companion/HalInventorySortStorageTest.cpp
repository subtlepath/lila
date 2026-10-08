#include <gtest/gtest.h>

#include <array>

#include "HalStorage.h"
#include "lib/Tinta/src/core/srs/Bytes.h"
#include "lib/hal/HalInventorySortStorage.h"
#include "lib/hal/HalJournalIdentityIndexSink.h"
#include "lib/hal/HalJournalIdentityIndexStorage.h"
#include "lib/hal/HalJournalIdentitySortStorage.h"
using namespace companion;
namespace {
class ReverseSource final : public UnsortedInventorySource {
 public:
  explicit ReverseSource(uint32_t count) : remaining(count) {}
  InventorySourceResult next(ContentManifest& manifest) override {
    if (remaining == 0) return InventorySourceResult::End;
    manifest = {};
    const auto value = --remaining;
    for (size_t byte = 0; byte < 4; ++byte)
      manifest.contentHash[byte] = static_cast<uint8_t>(value >> (8 * (3 - byte)));
    manifest.length = value + 1;
    return InventorySourceResult::Entry;
  }
  uint32_t remaining;
};
class HalInventorySortStorageTest : public testing::Test {
 protected:
  void SetUp() override { inventory_hal_test::state = {}; }
};
}  // namespace
TEST_F(HalInventorySortStorageTest, SortsMultiplePassesWithOnlyTwoReusableHalHandles) {
  HalInventorySortStorage storage;
  std::array<uint8_t, 201> scratch{};
  InventorySorter sorter(storage, scratch);
  for (const uint32_t count : {uint32_t{257}, uint32_t{0}, uint32_t{12}}) {
    ReverseSource source(count);
    ASSERT_TRUE(sorter.build(source));
    ContentManifest manifest;
    for (uint32_t index = 0; index < count; ++index) {
      ASSERT_EQ(sorter.next(manifest), InventorySourceResult::Entry);
      EXPECT_EQ(manifest.length, index + 1);
    }
    EXPECT_EQ(sorter.next(manifest), InventorySourceResult::End);
    EXPECT_EQ(inventory_hal_test::state.opens, 2);
  }
  EXPECT_GT(inventory_hal_test::state.yields, 0u);
  EXPECT_TRUE(storage.close());
  EXPECT_EQ(inventory_hal_test::state.closes, 2);
}
TEST_F(HalInventorySortStorageTest, RequiresFinishedRunBeforeReadingAndCanResetAfterFailure) {
  HalInventorySortStorage storage;
  ASSERT_TRUE(storage.reset());
  std::array<uint8_t, 4> bytes{1, 2, 3, 4};
  ASSERT_TRUE(storage.write(0, 0, bytes));
  EXPECT_FALSE(storage.read(0, 0, bytes));
  EXPECT_GT(inventory_hal_test::state.errors, 0);
  ASSERT_TRUE(storage.reset());
  ASSERT_TRUE(storage.write(0, 0, bytes));
  ASSERT_TRUE(storage.finish(0, bytes.size()));
  std::array<uint8_t, 4> decoded{};
  EXPECT_TRUE(storage.read(0, 0, decoded));
  EXPECT_EQ(decoded, bytes);
  EXPECT_EQ(inventory_hal_test::state.opens, 2);
}
TEST_F(HalInventorySortStorageTest, RestartingOutputTruncatesStaleTailAtFinish) {
  HalInventorySortStorage storage;
  ASSERT_TRUE(storage.reset());
  std::array<uint8_t, 8> first{};
  ASSERT_TRUE(storage.write(0, 0, first));
  ASSERT_TRUE(storage.finish(0, first.size()));
  std::array<uint8_t, 3> replacement{7, 8, 9};
  ASSERT_TRUE(storage.write(0, 0, replacement));
  ASSERT_TRUE(storage.finish(0, replacement.size()));
  EXPECT_EQ(inventory_hal_test::state.files.at(HalInventorySortStorage::RUN_PATHS[0]), std::vector<uint8_t>({7, 8, 9}));
}
TEST_F(HalInventorySortStorageTest, RejectsInvalidRunsOffsetsLengthsAndOverflow) {
  HalInventorySortStorage storage;
  std::array<uint8_t, 4> bytes{};
  ASSERT_TRUE(storage.reset());
  EXPECT_FALSE(storage.write(2, 0, bytes));
  ASSERT_TRUE(storage.reset());
  EXPECT_FALSE(storage.write(0, 1, bytes));
  ASSERT_TRUE(storage.reset());
  EXPECT_FALSE(storage.write(0, UINT64_MAX - 1, bytes));
  ASSERT_TRUE(storage.reset());
  ASSERT_TRUE(storage.write(0, 0, bytes));
  EXPECT_FALSE(storage.finish(0, 3));
  ASSERT_TRUE(storage.reset());
  ASSERT_TRUE(storage.write(0, 0, bytes));
  ASSERT_TRUE(storage.finish(0, 4));
  EXPECT_FALSE(storage.read(0, 3, bytes));
}
TEST_F(HalInventorySortStorageTest, OpenDirectoryAndResetFailuresPreventUsableRuns) {
  for (unsigned failure = 0; failure < 4; ++failure) {
    inventory_hal_test::state = {};
    auto& state = inventory_hal_test::state;
    state.failOpen = failure == 0;
    state.failDirectory = failure == 1;
    state.failTruncate = failure == 2;
    state.failSync = failure == 3;
    HalInventorySortStorage storage;
    EXPECT_FALSE(storage.reset());
    std::array<uint8_t, 1> byte{};
    EXPECT_FALSE(storage.write(0, 0, byte));
    EXPECT_GT(state.errors, 0);
  }
}
TEST_F(HalInventorySortStorageTest, WriteAndSyncFailuresDoNotExposeCompleteCatalog) {
  for (bool sync : {false, true}) {
    inventory_hal_test::state = {};
    HalInventorySortStorage storage;
    ASSERT_TRUE(storage.reset());
    if (sync)
      inventory_hal_test::state.failSync = true;
    else
      inventory_hal_test::state.failWrite = true;
    std::array<uint8_t, 4> bytes{};
    if (sync) {
      ASSERT_TRUE(storage.write(0, 0, bytes));
      EXPECT_FALSE(storage.finish(0, bytes.size()));
    } else
      EXPECT_FALSE(storage.write(0, 0, bytes));
    EXPECT_FALSE(storage.read(0, 0, bytes));
  }
}
TEST_F(HalInventorySortStorageTest, ShortReadInvalidatesPublishedSortConsumption) {
  HalInventorySortStorage storage;
  std::array<uint8_t, 201> scratch{};
  ReverseSource source(2);
  InventorySorter sorter(storage, scratch);
  ASSERT_TRUE(sorter.build(source));
  inventory_hal_test::state.shortRead = inventory_hal_test::state.reads + 1;
  ContentManifest manifest;
  EXPECT_EQ(sorter.next(manifest), InventorySourceResult::Error);
  EXPECT_EQ(sorter.next(manifest), InventorySourceResult::Error);
}
TEST_F(HalInventorySortStorageTest, CloseFailureIsReportedAndResourcesReleased) {
  HalInventorySortStorage storage;
  ASSERT_TRUE(storage.reset());
  inventory_hal_test::state.failClose = true;
  EXPECT_FALSE(storage.close());
  EXPECT_EQ(inventory_hal_test::state.closes, 2);
  EXPECT_EQ(inventory_hal_test::state.errors, 2);
}

TEST_F(HalInventorySortStorageTest, JournalRunsReuseHandlesWithoutChangingInventoryRuns) {
  class Source final : public JournalIdentitySource {
   public:
    uint32_t remaining = 257;
    JournalIdentitySourceResult next(JournalIdentityEntry& entry) override {
      if (!remaining) return JournalIdentitySourceResult::End;
      entry = {};
      entry.identity.origin.fill(1);
      entry.identity.epoch = remaining;
      entry.identity.sequence = 1;
      entry.record = --remaining;
      return JournalIdentitySourceResult::Entry;
    }
  } source;
  auto& state = inventory_hal_test::state;
  for (const auto& path : HalInventorySortStorage::RUN_PATHS) state.files[path] = {9};
  HalJournalIdentitySortStorage storage;
  std::array<uint8_t, 120> scratch{};
  JournalIdentitySorter sorter(storage, scratch);
  ASSERT_TRUE(sorter.build(source));
  JournalIdentityEntry entry;
  for (unsigned at = 0; at < 257; ++at) {
    ASSERT_EQ(sorter.next(entry), JournalIdentitySourceResult::Entry);
    EXPECT_EQ(entry.identity.epoch, at + 1);
    EXPECT_EQ(entry.record, at);
  }
  EXPECT_EQ(sorter.next(entry), JournalIdentitySourceResult::End);
  EXPECT_EQ(state.opens, 2);
  EXPECT_GT(state.yields, 0u);
  for (const auto& path : HalInventorySortStorage::RUN_PATHS) EXPECT_EQ(state.files[path], (std::vector<uint8_t>{9}));
  EXPECT_TRUE(storage.close());
  EXPECT_EQ(state.closes, 2);
}
TEST_F(HalInventorySortStorageTest, InvalidOrAliasedRunPathsCannotOpenFiles) {
  for (unsigned at = 0; at < 2; ++at) {
    HalInventorySortStorage storage(at ? "same" : nullptr, "same");
    EXPECT_FALSE(storage.reset());
    EXPECT_EQ(inventory_hal_test::state.opens, 0);
  }
}

TEST_F(HalInventorySortStorageTest, JournalIndexReadsVerifiedEntriesThroughOneHandleAndLatchesReadFailure) {
  auto& state = inventory_hal_test::state;
  std::array<uint8_t, JOURNAL_IDENTITY_HEADER_SIZE + 2 * JOURNAL_IDENTITY_ENTRY_SIZE> bytes{};
  JournalIdentityEntry entry;
  entry.identity.origin.fill(1);
  entry.identity.epoch = 1;
  for (unsigned at = 0; at < 2; ++at) {
    entry.identity.sequence = at + 1;
    entry.record = 1 - at;
    ASSERT_TRUE(encodeJournalIdentityEntry(
        entry, std::span(bytes).subspan(JOURNAL_IDENTITY_HEADER_SIZE + at * JOURNAL_IDENTITY_ENTRY_SIZE,
                                        JOURNAL_IDENTITY_ENTRY_SIZE)));
  }
  ASSERT_TRUE(encodeJournalIdentityHeader(
      2, tinta::core::crc32(bytes.data() + JOURNAL_IDENTITY_HEADER_SIZE, bytes.size() - JOURNAL_IDENTITY_HEADER_SIZE),
      std::span(bytes).first(JOURNAL_IDENTITY_HEADER_SIZE)));
  state.files[HalJournalIdentityIndexStorage::PATH] = std::vector<uint8_t>(bytes.begin(), bytes.end());
  HalJournalIdentityIndexStorage storage;
  ASSERT_TRUE(storage.open());
  std::array<uint8_t, JOURNAL_IDENTITY_ENTRY_SIZE> scratch{};
  IndexedJournalIdentities index(storage, scratch);
  ASSERT_TRUE(index.open(2));
  uint32_t record = UINT32_MAX;
  ASSERT_EQ(index.find(entry.identity, record), JournalIdentityLookup::Found);
  EXPECT_EQ(record, 0u);
  EXPECT_EQ(state.readOpens, 1);
  state.failRead = state.reads + 1;
  EXPECT_EQ(index.find(entry.identity, record), JournalIdentityLookup::IoError);
  EXPECT_TRUE(storage.hadReadError());
  EXPECT_EQ(index.find(entry.identity, record), JournalIdentityLookup::IoError);
  EXPECT_EQ(record, 0u);
  EXPECT_TRUE(storage.close());
  EXPECT_EQ(state.closes, 1);
}

TEST_F(HalInventorySortStorageTest, JournalIndexPublicationRecoversRenameFailuresWithoutChangingJournal) {
  class Source final : public JournalIdentitySource {
   public:
    unsigned cursor = 0;
    JournalIdentitySourceResult next(JournalIdentityEntry& entry) override {
      if (cursor == 2) return JournalIdentitySourceResult::End;
      entry = {};
      entry.identity.origin.fill(1);
      entry.identity.epoch = 1;
      entry.identity.sequence = cursor + 1;
      entry.record = cursor++;
      return JournalIdentitySourceResult::Entry;
    }
  };
  auto& state = inventory_hal_test::state;
  state.enumerateFileMap = true;
  state.files["/.crosspoint/companion/history-sentinel"] = {1, 2, 3};
  std::array<uint8_t, 40> scratch{};
  auto build = [&] {
    Source source;
    HalJournalIdentityIndexSink sink(2, scratch);
    JournalIdentityIndexBuilder builder(sink, scratch);
    return builder.build(source, 2);
  };
  ASSERT_TRUE(build());
  const auto original = state;
  for (unsigned fault = 0; fault < 6; ++fault) {
    state = original;
    state.renames = 0;
    state.failSync = fault == 0;
    state.failRead = fault == 1 ? state.reads + 1 : 0;
    state.failRename = fault == 2 ? 1 : fault == 3 ? 2 : 0;
    state.failRenameAfter = fault == 4 ? 1 : fault == 5 ? 2 : 0;
    EXPECT_FALSE(build());
    state.failSync = false;
    state.failRead = state.failRename = state.failRenameAfter = 0;
    ASSERT_TRUE(build());
    EXPECT_EQ(state.files[HalJournalIdentityIndexSink::ACTIVE], original.files.at(HalJournalIdentityIndexSink::ACTIVE));
    EXPECT_EQ(state.files["/.crosspoint/companion/history-sentinel"], (std::vector<uint8_t>{1, 2, 3}));
    HalJournalIdentityIndexStorage storage;
    ASSERT_TRUE(storage.open());
    IndexedJournalIdentities index(storage, scratch);
    EXPECT_TRUE(index.open(2));
    EXPECT_TRUE(storage.close());
  }
}

#include <gtest/gtest.h>

#include "lib/hal/HalInventoryIndexSink.h"
using namespace companion;
using inventory_hal_test::state;
namespace companion {
bool HalTransferStorage::prepare() { return true; }
FileStatus HalTransferStorage::stat(const char* path, uint64_t& bytes) {
  if (state.statErrorPath == path) return FileStatus::Error;
  auto it = state.files.find(path);
  if (it == state.files.end()) return FileStatus::Missing;
  bytes = it->second.size();
  return FileStatus::Present;
}
bool HalTransferStorage::rename(const char* from, const char* to) {
  if ((++state.renames == state.failRename || state.renames == state.failRenameAgain) || !state.files.contains(from) ||
      state.files.contains(to))
    return false;
  state.files[to] = std::move(state.files.at(from));
  state.files.erase(from);
  return true;
}
bool HalTransferStorage::remove(const char* path) { return state.files.erase(path) == 1; }
bool HalTransferStorage::read(const char*, uint64_t, std::span<uint8_t>) { return false; }
bool HalTransferStorage::write(const char*, uint64_t, std::span<const uint8_t>, bool) { return false; }
bool HalTransferStorage::resize(const char*, uint64_t) { return false; }
bool HalTransferStorage::validateContent(const char*, const char*, const ContentManifest&, std::span<uint8_t>) {
  return false;
}
bool HalTransferStorage::installContentMetadata(const char*, const ContentManifest&, std::span<uint8_t>) {
  return false;
}
bool HalTransferStorage::validateContent(const char*, const char*, const ContentManifest&, const TransferState&,
                                         std::span<uint8_t>) {
  return false;
}
bool HalTransferStorage::installContentMetadata(const char*, const ContentManifest&, const TransferState&,
                                                std::span<uint8_t>) {
  return false;
}
bool HalTransferStorage::finalizeContentMetadata(const char*, const ContentManifest&, const TransferState&,
                                                 std::span<uint8_t>) {
  return false;
}
bool HalTransferStorage::installDictionaryMembers(const char*, const ContentManifest&, const TransferState&,
                                                  std::span<uint8_t>) {
  return false;
}
bool HalTransferStorage::verifyDictionaryArchive(const char*, const ContentManifest&, const TransferState&,
                                                 std::span<uint8_t>) {
  return false;
}
bool HalTransferStorage::verify(const char*, uint64_t, const Digest&, std::span<uint8_t>) { return false; }
}  // namespace companion
namespace {
constexpr char NEXT[] = "/.crosspoint/companion/inventory-next";
constexpr char OLD[] = "/.crosspoint/companion/inventory-old";
Identity generation() {
  Identity id{};
  id[0] = 1;
  return id;
}
std::vector<uint8_t> snapshot(uint64_t revision) {
  std::vector<uint8_t> bytes(INVENTORY_INDEX_HEADER_SIZE);
  EXPECT_EQ(encodeInventoryIndexHeader({generation(), revision, 0, 0}, bytes), bytes.size());
  return bytes;
}
class InventorySinkHal : public testing::Test {
 protected:
  bool start(HalInventoryIndexSink& sink) {
    uint64_t revision = 0;
    return sink.nextRevision(revision) && sink.begin();
  }

 private:
  void SetUp() override { state = inventory_hal_test::State{}; }
};
}  // namespace
TEST_F(InventorySinkHal, PublishesAndRetainsBackupUntilRecovery) {
  auto old = snapshot(1), next = snapshot(2);
  state.files[HalInventoryIndexSink::ACTIVE] = old;
  std::array<uint8_t, 67> scratch{};
  HalInventoryIndexSink sink(generation(), scratch);
  ASSERT_TRUE(start(sink));
  ASSERT_TRUE(sink.write(0, next));
  ASSERT_TRUE(sink.finish(next.size()));
  EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), next);
  EXPECT_EQ(state.files.at(OLD), old);
  ASSERT_TRUE(sink.recover());
  EXPECT_FALSE(state.files.contains(OLD));
}
TEST_F(InventorySinkHal, FailedSyncWriteAndRenamesPreservePreviousSnapshot) {
  for (unsigned failure = 0; failure < 4; ++failure) {
    state = inventory_hal_test::State{};
    auto old = snapshot(1), next = snapshot(2);
    state.files[HalInventoryIndexSink::ACTIVE] = old;
    std::array<uint8_t, 67> scratch{};
    HalInventoryIndexSink sink(generation(), scratch);
    ASSERT_TRUE(start(sink));
    state.failWrite = failure == 0;
    bool written = sink.write(0, next);
    state.failSync = failure == 1;
    state.failRename = failure >= 2 ? failure - 1 : 0;
    EXPECT_FALSE(written && sink.finish(next.size()));
    sink.abort();
    EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), old);
    EXPECT_FALSE(state.files.contains(NEXT));
  }
}
TEST_F(InventorySinkHal, RecoversInterruptedRenamesAndIgnoresAbandonedCandidate) {
  for (bool published : {false, true}) {
    state = inventory_hal_test::State{};
    auto old = snapshot(1), next = snapshot(2);
    state.files[OLD] = old;
    state.files[published ? HalInventoryIndexSink::ACTIVE : NEXT] = next;
    std::array<uint8_t, 67> scratch{};
    HalInventoryIndexSink sink(generation(), scratch);
    ASSERT_TRUE(sink.recover());
    EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), published ? next : old);
    EXPECT_FALSE(state.files.contains(OLD));
    EXPECT_FALSE(state.files.contains(NEXT));
  }
}
TEST_F(InventorySinkHal, RejectsCorruptCandidateAndRestoresValidBackup) {
  auto old = snapshot(1);
  state.files[HalInventoryIndexSink::ACTIVE] = old;
  std::array<uint8_t, 67> scratch{};
  HalInventoryIndexSink sink(generation(), scratch);
  ASSERT_TRUE(start(sink));
  auto corrupt = snapshot(2);
  corrupt[44] ^= 1;
  ASSERT_TRUE(sink.write(0, corrupt));
  EXPECT_FALSE(sink.finish(corrupt.size()));
  sink.abort();
  EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), old);
  state.files[OLD] = old;
  state.files[HalInventoryIndexSink::ACTIVE] = corrupt;
  ASSERT_TRUE(sink.recover());
  EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), old);
}

TEST_F(InventorySinkHal, FailedImmediateRestoreRetainsBackupForLaterRecovery) {
  auto old = snapshot(1), next = snapshot(2);
  state.files[HalInventoryIndexSink::ACTIVE] = old;
  std::array<uint8_t, 67> scratch{};
  HalInventoryIndexSink sink(generation(), scratch);
  ASSERT_TRUE(start(sink));
  ASSERT_TRUE(sink.write(0, next));
  state.failRename = 2;
  state.failRenameAgain = 3;
  EXPECT_FALSE(sink.finish(next.size()));
  sink.abort();
  EXPECT_FALSE(state.files.contains(HalInventoryIndexSink::ACTIVE));
  EXPECT_EQ(state.files.at(OLD), old);
  state.failRename = state.failRenameAgain = 0;
  ASSERT_TRUE(sink.recover());
  EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), old);
}

TEST_F(InventorySinkHal, RefusesRecoveryWithoutAnyValidSnapshot) {
  state.files[HalInventoryIndexSink::ACTIVE] = {1, 2, 3};
  state.files[OLD] = {4, 5, 6};
  std::array<uint8_t, 67> scratch{};
  HalInventoryIndexSink sink(generation(), scratch);
  EXPECT_FALSE(sink.recover());
  EXPECT_FALSE(sink.begin());
  EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), (std::vector<uint8_t>{1, 2, 3}));
  EXPECT_EQ(state.files.at(OLD), (std::vector<uint8_t>{4, 5, 6}));
}

TEST_F(InventorySinkHal, MetadataErrorsPreventRecoveryMutations) {
  for (const char* path : {HalInventoryIndexSink::ACTIVE, OLD}) {
    state = inventory_hal_test::State{};
    state.files[HalInventoryIndexSink::ACTIVE] = snapshot(2);
    state.files[OLD] = snapshot(1);
    state.files[NEXT] = snapshot(3);
    const auto originals = state.files;
    state.statErrorPath = path;
    std::array<uint8_t, 67> scratch{};
    HalInventoryIndexSink sink(generation(), scratch);
    EXPECT_FALSE(sink.recover());
    EXPECT_EQ(state.files, originals);
    EXPECT_EQ(state.renames, 0);
  }
}

TEST_F(InventorySinkHal, FailedCandidateCloseCannotPublish) {
  const auto old = snapshot(1), next = snapshot(2);
  state.files[HalInventoryIndexSink::ACTIVE] = old;
  std::array<uint8_t, 67> scratch{};
  HalInventoryIndexSink sink(generation(), scratch);
  ASSERT_TRUE(start(sink));
  ASSERT_TRUE(sink.write(0, next));
  state.failClose = true;
  EXPECT_FALSE(sink.finish(next.size()));
  EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), old);
  EXPECT_EQ(state.renames, 0);
  sink.abort();
  EXPECT_FALSE(state.files.contains(NEXT));
}

TEST_F(InventorySinkHal, ReadErrorsDoNotTriggerRollbackOrDiscardSnapshots) {
  for (const char* path : {HalInventoryIndexSink::ACTIVE, OLD}) {
    state = inventory_hal_test::State{};
    state.files[HalInventoryIndexSink::ACTIVE] = snapshot(2);
    state.files[OLD] = snapshot(1);
    state.files[NEXT] = snapshot(3);
    if (std::string(path) == OLD) state.files[HalInventoryIndexSink::ACTIVE][44] ^= 1;
    const auto originals = state.files;
    state.readErrorPath = path;
    std::array<uint8_t, 67> scratch{};
    HalInventoryIndexSink sink(generation(), scratch);
    EXPECT_FALSE(sink.recover());
    EXPECT_EQ(state.files, originals);
    EXPECT_EQ(state.renames, 0);
    state.readErrorPath.clear();
    ASSERT_TRUE(sink.recover());
    EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), snapshot(std::string(path) == OLD ? 1 : 2));
  }
}

TEST_F(InventorySinkHal, RejectsReusedAndOlderRevisionsBeforeRenaming) {
  for (uint64_t revision : {1ULL, 2ULL}) {
    state = inventory_hal_test::State{};
    const auto old = snapshot(2), next = snapshot(revision);
    state.files[HalInventoryIndexSink::ACTIVE] = old;
    std::array<uint8_t, 67> scratch{};
    HalInventoryIndexSink sink(generation(), scratch);
    ASSERT_TRUE(start(sink));
    ASSERT_TRUE(sink.write(0, next));
    EXPECT_FALSE(sink.finish(next.size()));
    EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), old);
    EXPECT_EQ(state.renames, 0);
  }
}

TEST_F(InventorySinkHal, NextRevisionUsesRecoveredSnapshotAndRejectsOverflow) {
  std::array<uint8_t, 67> scratch{};
  HalInventoryIndexSink sink(generation(), scratch);
  uint64_t revision = 99;
  ASSERT_TRUE(sink.nextRevision(revision));
  EXPECT_EQ(revision, 1);
  state.files[OLD] = snapshot(7);
  ASSERT_TRUE(sink.nextRevision(revision));
  EXPECT_EQ(revision, 8);
  state.files[HalInventoryIndexSink::ACTIVE] = snapshot(UINT64_MAX);
  EXPECT_FALSE(sink.nextRevision(revision));
  EXPECT_EQ(revision, 8);
  EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), snapshot(UINT64_MAX));
}

TEST_F(InventorySinkHal, RevisionReservationsSurviveSnapshotRollbackAndFailedWrite) {
  std::array<uint8_t, 67> scratch{};
  HalInventoryIndexSink sink(generation(), scratch);
  state.files[HalInventoryIndexSink::ACTIVE] = snapshot(10);
  uint64_t revision = 0;
  ASSERT_TRUE(sink.nextRevision(revision));
  EXPECT_EQ(revision, 11);
  state.files[HalInventoryIndexSink::ACTIVE] = snapshot(3);
  ASSERT_TRUE(sink.nextRevision(revision));
  EXPECT_EQ(revision, 12);
  state.failNvsWrite = true;
  EXPECT_FALSE(sink.nextRevision(revision));
  EXPECT_EQ(revision, 12);
  state.failNvsWrite = false;
  ASSERT_TRUE(sink.nextRevision(revision));
  EXPECT_EQ(revision, 13);
}
TEST_F(InventorySinkHal, MalformedReservationIsNotReset) {
  HalInventoryRevisions revisions;
  uint64_t output = 99;
  for (auto invalid : {std::vector<uint8_t>{1}, std::vector<uint8_t>(8, 0)}) {
    state.revisionBytes = invalid;
    EXPECT_FALSE(revisions.reserve(0, output));
    EXPECT_EQ(output, 99);
    EXPECT_EQ(state.revisionBytes, invalid);
  }
}

TEST_F(InventorySinkHal, PublicationRequiresReservedRevision) {
  std::array<uint8_t, 67> scratch{};
  HalInventoryIndexSink sink(generation(), scratch);
  EXPECT_FALSE(sink.begin());
  EXPECT_TRUE(state.files.empty());
  uint64_t revision = 0;
  ASSERT_TRUE(sink.nextRevision(revision));
  ASSERT_TRUE(sink.begin());
  const auto unreserved = snapshot(revision + 1);
  ASSERT_TRUE(sink.write(0, unreserved));
  EXPECT_FALSE(sink.finish(unreserved.size()));
  EXPECT_FALSE(state.files.contains(HalInventoryIndexSink::ACTIVE));
  sink.abort();
  ASSERT_TRUE(sink.nextRevision(revision));
  EXPECT_EQ(revision, 2);
}

TEST_F(InventorySinkHal, FailedReadbackConsumesReservationWithoutReturningIt) {
  HalInventoryRevisions revisions;
  uint64_t output = 99;
  state.failNvsRead = true;
  EXPECT_FALSE(revisions.reserve(0, output));
  EXPECT_EQ(output, 99);
  ASSERT_EQ(state.revisionBytes.size(), 8);
  EXPECT_EQ(state.revisionBytes[0], 1);
  state.failNvsRead = false;
  ASSERT_TRUE(revisions.reserve(0, output));
  EXPECT_EQ(output, 2);
  state.failNvsOpen = true;
  EXPECT_FALSE(revisions.reserve(0, output));
  EXPECT_EQ(output, 2);
}

namespace {
struct FullInventorySource : SortedInventorySource {
  std::vector<ContentManifest> entries;
  size_t cursor = 0;
  bool failEnd = false;
  InventorySourceResult next(ContentManifest& manifest) override {
    if (cursor == entries.size()) return failEnd ? InventorySourceResult::Error : InventorySourceResult::End;
    manifest = entries[cursor++];
    return InventorySourceResult::Entry;
  }
};
}  // namespace
TEST_F(InventorySinkHal, BuildsPublishesAndReadsEveryContentKindThroughHal) {
  FullInventorySource source;
  source.entries.reserve(5);
  for (uint8_t kind = 1; kind <= 5; ++kind) {
    ContentManifest manifest;
    manifest.contentHash[0] = kind;
    manifest.kind = static_cast<ContentKind>(kind);
    manifest.length = kind * 100;
    source.entries.push_back(manifest);
  }
  std::array<uint8_t, 67> scratch{};
  HalInventoryIndexSink sink(generation(), scratch);
  uint64_t revision = 99;
  ASSERT_TRUE(sink.buildAndPublish(source, revision));
  EXPECT_EQ(revision, 1);
  HalInventoryIndexStorage storage;
  ASSERT_TRUE(storage.open(HalInventoryIndexSink::ACTIVE));
  IndexedInventoryCatalog catalog(storage, scratch);
  ASSERT_TRUE(catalog.open(generation()));
  EXPECT_EQ(catalog.revision(), revision);
  ASSERT_EQ(catalog.count(), 5);
  for (uint64_t index = 0; index < catalog.count(); ++index) {
    ContentManifest manifest;
    ASSERT_TRUE(catalog.read(index, manifest));
    EXPECT_EQ(manifest, source.entries[index]);
  }
}
TEST_F(InventorySinkHal, FailedScanRetainsSnapshotAndConsumesReservedRevision) {
  FullInventorySource source;
  source.failEnd = true;
  state.files[HalInventoryIndexSink::ACTIVE] = snapshot(5);
  std::array<uint8_t, 67> scratch{};
  HalInventoryIndexSink sink(generation(), scratch);
  uint64_t revision = 99;
  EXPECT_FALSE(sink.buildAndPublish(source, revision));
  EXPECT_EQ(revision, 99);
  EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), snapshot(5));
  EXPECT_FALSE(state.files.contains(NEXT));
  source.failEnd = false;
  ASSERT_TRUE(sink.buildAndPublish(source, revision));
  EXPECT_EQ(revision, 7);
  EXPECT_EQ(state.files.at(HalInventoryIndexSink::ACTIVE), snapshot(7));
}

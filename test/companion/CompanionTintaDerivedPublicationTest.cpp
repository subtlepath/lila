#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <vector>

#include "lib/Companion/CompanionTintaDerivedPaths.h"
#include "lib/Companion/CompanionTintaDerivedPublication.h"
#include "lib/Companion/CompanionTintaReceiveCheckpoint.h"
#include "lib/Companion/CompanionTintaReceiveJournal.h"
#include "lib/Tinta/src/core/srs/Bytes.h"

using namespace companion;
TEST(TintaDerivedPaths, AllRolesAreDistinctCourseScopedAndAvoidLocalTemporaryFiles) {
  Identity course{};
  course.fill(7);
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  std::vector<std::string> names;
  names.reserve(19);
  for (unsigned file = 0; file < 5; ++file)
    for (unsigned role = 0; role < 3; ++role) {
      ASSERT_TRUE(
          tintaDerivedFilePath(course, static_cast<TintaDerivedFile>(file), static_cast<TintaDerivedRole>(role), path));
      names.emplace_back(path.data());
    }
  for (unsigned record = 0; record < 4; ++record) {
    ASSERT_TRUE(tintaDerivedRecordPath(course, static_cast<TintaDerivedRecord>(record), path));
    names.emplace_back(path.data());
  }
  for (size_t i = 0; i < names.size(); ++i) {
    EXPECT_TRUE(names[i].starts_with("/tinta/courses/07070707070707070707070707070707/"));
    EXPECT_FALSE(names[i].ends_with(".tmp"));
    EXPECT_FALSE(names[i].ends_with("/read.bin"));
    for (size_t j = 0; j < i; ++j) EXPECT_NE(names[i], names[j]);
  }
  EXPECT_FALSE(tintaDerivedFilePath(course, static_cast<TintaDerivedFile>(5), TintaDerivedRole::Active, path));
  EXPECT_EQ(path[0], '\0');
  EXPECT_FALSE(tintaDerivedFilePath(course, TintaDerivedFile::Items, static_cast<TintaDerivedRole>(3), path));
  EXPECT_FALSE(tintaDerivedRecordPath(course, static_cast<TintaDerivedRecord>(4), path));
  EXPECT_FALSE(tintaDerivedRecordPath({}, TintaDerivedRecord::Intent, path));
  EXPECT_FALSE(tintaDerivedRecordPath(course, TintaDerivedRecord::Intent, std::span(path).first(8)));
}
namespace {
class Store final : public TintaDerivedPublicationStorage {
 public:
  std::array<std::array<TintaPublicationState, 3>, 5> files{};
  TintaPublicationState pending = TintaPublicationState::Missing, receipt = TintaPublicationState::Missing;
  unsigned operations = 0, failAt = 0;
  bool after = false, valid = true;
  Store() {
    for (auto& file : files)
      file = {TintaPublicationState::Other, TintaPublicationState::Matches, TintaPublicationState::Missing};
  }
  template <class Action>
  bool change(Action action) {
    ++operations;
    if (operations == failAt && !after) return false;
    action();
    return operations != failAt;
  }
  bool validateBindings(const TintaDerivedManifestView&) override { return true; }
  bool validateCandidates(const TintaDerivedManifestView&) override { return valid; }
  TintaPublicationState intent(std::span<const uint8_t>) override { return pending; }
  TintaPublicationState committed(std::span<const uint8_t>) override { return receipt; }
  TintaPublicationState state(TintaDerivedFile file, TintaDerivedRole role, const TintaDerivedManifestView&) override {
    return files[static_cast<unsigned>(file)][static_cast<unsigned>(role)];
  }
  bool persistIntent(std::span<const uint8_t>) override {
    return change([&] { pending = TintaPublicationState::Matches; });
  }
  bool rename(TintaDerivedFile file, TintaDerivedRole from, TintaDerivedRole to) override {
    EXPECT_EQ(pending, TintaPublicationState::Matches);
    auto& paths = files[static_cast<unsigned>(file)];
    EXPECT_EQ(paths[static_cast<unsigned>(to)], TintaPublicationState::Missing);
    return change([&] {
      paths[static_cast<unsigned>(to)] = paths[static_cast<unsigned>(from)];
      paths[static_cast<unsigned>(from)] = TintaPublicationState::Missing;
    });
  }
  bool remove(TintaDerivedFile file, TintaDerivedRole role) override {
    if (role == TintaDerivedRole::Backup) EXPECT_EQ(receipt, TintaPublicationState::Matches);
    return change(
        [&] { files[static_cast<unsigned>(file)][static_cast<unsigned>(role)] = TintaPublicationState::Missing; });
  }
  bool commit(std::span<const uint8_t>) override {
    for (const auto& file : files) EXPECT_EQ(file[0], TintaPublicationState::Matches);
    return change([&] { receipt = TintaPublicationState::Matches; });
  }
  bool clearIntent() override {
    return change([&] { pending = TintaPublicationState::Missing; });
  }
};
std::vector<uint8_t> fixture() {
  std::ifstream input(TINTA_DERIVED_FIXTURE, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
}  // namespace
TEST(TintaDerivedPublication, EveryMutationCutRecoversOneGenerationAndRepeatedCommitDoesNothing) {
  const auto bytes = fixture();
  ASSERT_EQ(bytes.size(), TINTA_DERIVED_MANIFEST_SIZE);
  for (unsigned cut = 1; cut <= 18; ++cut)
    for (bool after : {false, true}) {
      Store store;
      store.failAt = cut;
      store.after = after;
      TintaDerivedPublication publication(store);
      EXPECT_EQ(publication.publish(bytes), TintaPublicationResult::IoError);
      store.failAt = 0;
      TintaDerivedPublication restarted(store);
      ASSERT_EQ(restarted.publish(bytes), TintaPublicationResult::Ok);
      for (const auto& file : store.files) {
        EXPECT_EQ(file[0], TintaPublicationState::Matches);
        EXPECT_EQ(file[1], TintaPublicationState::Missing);
        EXPECT_EQ(file[2], TintaPublicationState::Missing);
      }
      EXPECT_EQ(store.pending, TintaPublicationState::Missing);
      const auto operations = store.operations;
      EXPECT_EQ(restarted.publish(bytes), TintaPublicationResult::Ok);
      EXPECT_EQ(store.operations, operations);
    }
}
TEST(TintaDerivedPublication, InvalidCandidatesAndAmbiguousBackupsCannotStartPublication) {
  const auto bytes = fixture();
  Store store;
  store.valid = false;
  TintaDerivedPublication publication(store);
  EXPECT_EQ(publication.publish(bytes), TintaPublicationResult::Invalid);
  EXPECT_EQ(store.operations, 0u);
  store.valid = true;
  store.files[0][2] = TintaPublicationState::Other;
  EXPECT_EQ(publication.publish(bytes), TintaPublicationResult::Corrupt);
  EXPECT_EQ(store.operations, 0u);
}

TEST(TintaReceiveCheckpoint, RequiresExactlyThePrecedingSealedFilesEvenWithValidCrc) {
  companion::TintaReceiveCheckpoint value;
  value.course.fill(1);
  value.transaction.fill(2);
  value.owner.fill(3);
  value.storage.fill(4);
  value.manifestHash.fill(5);
  value.sequence = 1;
  std::array<uint8_t, companion::TINTA_RECEIVE_CHECKPOINT_SIZE> bytes{};
  for (unsigned file = 0; file < 5; ++file) {
    value.file = static_cast<companion::TintaDerivedFile>(file);
    value.sealedMask = static_cast<uint8_t>((1u << file) - 1);
    ASSERT_TRUE(companion::encodeTintaReceiveCheckpoint(value, bytes));
    for (unsigned mask = 0; mask < 32; ++mask) {
      SCOPED_TRACE(testing::Message() << "file " << file << " mask " << mask);
      bytes[101] = mask;
      tinta::core::putU32(bytes.data() + 128, tinta::core::crc32(bytes.data(), 128));
      auto output = value;
      EXPECT_EQ(companion::decodeTintaReceiveCheckpoint(bytes, output), mask == value.sealedMask);
      EXPECT_EQ(output, value);
    }
  }
}
TEST(TintaReceiveCheckpoint, RoundTripRejectsCorruptionAndInvalidOwnershipOrOffsets) {
  companion::TintaReceiveCheckpoint value;
  value.course.fill(1);
  value.transaction.fill(2);
  value.owner.fill(3);
  value.storage.fill(4);
  value.manifestHash.fill(5);
  value.file = companion::TintaDerivedFile::Days;
  value.sealedMask = 15;
  value.offset = 3;
  value.length = 4;
  value.sequence = 256;
  std::array<uint8_t, companion::TINTA_RECEIVE_CHECKPOINT_SIZE> bytes{};
  ASSERT_TRUE(companion::encodeTintaReceiveCheckpoint(value, bytes));
  companion::TintaReceiveCheckpoint output;
  ASSERT_TRUE(companion::decodeTintaReceiveCheckpoint(bytes, output));
  EXPECT_EQ(output, value);
  for (size_t at = 0; at < bytes.size(); ++at) {
    auto corrupt = bytes;
    corrupt[at] ^= 1;
    EXPECT_FALSE(companion::decodeTintaReceiveCheckpoint(corrupt, output));
    EXPECT_EQ(output, value);
  }
  for (size_t length = 0; length < bytes.size(); ++length)
    EXPECT_FALSE(companion::decodeTintaReceiveCheckpoint(std::span(bytes).first(length), output));
  value.offset = 5;
  EXPECT_FALSE(companion::encodeTintaReceiveCheckpoint(value, bytes));
  value.offset = 3;
  value.sealedMask = 31;
  EXPECT_FALSE(companion::encodeTintaReceiveCheckpoint(value, bytes));
  value.sealedMask = 15;
  value.owner.fill(0);
  EXPECT_FALSE(companion::encodeTintaReceiveCheckpoint(value, bytes));
}

namespace {
struct ReceiveSlots : TintaReceiveJournalStorage {
  std::array<std::vector<uint8_t>, 2> slots;
  size_t cut = SIZE_MAX;
  bool failRead = false;
  ReceiveSlots() {
    for (auto& slot : slots) slot.reserve(TINTA_RECEIVE_CHECKPOINT_SIZE);
  }
  bool read(uint8_t slot, std::span<uint8_t> bytes, size_t& length) override {
    if (failRead) return false;
    length = slots[slot].size();
    std::copy(slots[slot].begin(), slots[slot].end(), bytes.begin());
    return true;
  }
  bool write(uint8_t slot, std::span<const uint8_t> bytes) override {
    slots[slot].assign(bytes.begin(), bytes.begin() + std::min(cut, bytes.size()));
    return cut == SIZE_MAX;
  }
};
}  // namespace
TEST(TintaReceiveJournal, EveryCheckpointWriteCutRecoversOldOrNewOffsetWithoutFurtherUncertainWrites) {
  TintaReceiveCheckpoint initial;
  initial.course.fill(1);
  initial.transaction.fill(2);
  initial.owner.fill(3);
  initial.storage.fill(4);
  initial.manifestHash.fill(5);
  initial.length = 100;
  initial.sequence = 1;
  auto next = initial;
  next.sequence = 2;
  next.offset = 37;
  for (size_t cut = 0; cut <= TINTA_RECEIVE_CHECKPOINT_SIZE; ++cut) {
    ReceiveSlots storage;
    std::array<uint8_t, TINTA_RECEIVE_CHECKPOINT_SIZE> scratch{};
    TintaReceiveJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaReceiveJournalResult::Missing);
    ASSERT_EQ(journal.start(initial), TintaReceiveJournalResult::Ok);
    storage.cut = cut;
    EXPECT_EQ(journal.advance(next), TintaReceiveJournalResult::IoError);
    EXPECT_EQ(journal.checkpoint(), nullptr);
    EXPECT_EQ(journal.advance(next), TintaReceiveJournalResult::Unavailable);
    storage.cut = SIZE_MAX;
    TintaReceiveJournal reopened(storage, scratch);
    ASSERT_EQ(reopened.open(), TintaReceiveJournalResult::Ok);
    EXPECT_EQ(*reopened.checkpoint(), cut == TINTA_RECEIVE_CHECKPOINT_SIZE ? next : initial);
    EXPECT_EQ(reopened.advance(next), TintaReceiveJournalResult::Ok);
    EXPECT_EQ(*reopened.checkpoint(), next);
    storage.failRead = true;
    EXPECT_EQ(reopened.open(), TintaReceiveJournalResult::IoError);
    EXPECT_EQ(reopened.checkpoint(), nullptr);
  }
}

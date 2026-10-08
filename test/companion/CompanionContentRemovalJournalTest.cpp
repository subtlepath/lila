#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/Companion/CompanionContentRemovalJournal.h"
using namespace companion;
namespace {
struct StorageFake final : ContentRemovalJournalStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  unsigned operations = 0, failAt = 0, writes = 0;
  bool afterEffect = false, corruptWrite = false;
  bool fail() { return ++operations == failAt; }
  bool prepare() override { return !fail(); }
  FileStatus stat(const char* path, uint64_t& size) override {
    if (fail()) return FileStatus::Error;
    if (!files.contains(path)) return FileStatus::Missing;
    size = files[path].size();
    return FileStatus::Present;
  }
  bool read(const char* path, std::span<uint8_t> bytes) override {
    if (fail() || !files.contains(path) || files[path].size() != bytes.size()) return false;
    std::copy(files[path].begin(), files[path].end(), bytes.begin());
    return true;
  }
  bool write(const char* path, std::span<const uint8_t> bytes) override {
    ++writes;
    const bool failed = fail();
    if (failed && !afterEffect) return false;
    files[path] = {bytes.begin(), bytes.end()};
    if (corruptWrite) files[path][20] ^= 1;
    return !failed;
  }
};
ContentRemovalRecord initial() {
  ContentRemovalRecord record;
  record.request.transaction.fill(1);
  record.request.owner.fill(2);
  record.request.generation.fill(3);
  record.request.manifest.contentHash.fill(4);
  record.request.manifest.kind = ContentKind::Dictionary;
  record.request.manifest.length = 1234;
  record.request.manifest.formatVersion = 1;
  record.planHash.fill(5);
  return record;
}
std::vector<uint8_t> encoded(const ContentRemovalRecord& record) {
  std::vector<uint8_t> bytes(CONTENT_REMOVAL_RECORD_SIZE);
  EXPECT_EQ(encodeContentRemovalRecord(record, bytes), bytes.size());
  return bytes;
}
}  // namespace
TEST(ContentRemovalJournal, CodecIntegrityAndSemanticChecksPreserveOutput) {
  const auto expected = initial();
  const auto bytes = encoded(expected);
  ContentRemovalRecord output = expected;
  for (size_t at = 0; at < bytes.size(); ++at) {
    auto corrupt = bytes;
    corrupt[at] ^= 1;
    EXPECT_FALSE(decodeContentRemovalRecord(corrupt, output));
    EXPECT_EQ(output, expected);
  }
  for (size_t size = 0; size < bytes.size(); ++size)
    EXPECT_FALSE(decodeContentRemovalRecord(std::span(bytes).first(size), output));
  for (const size_t at : {5U, 155U, 156U}) {
    auto invalid = bytes;
    invalid[at] = 9;
    inventory_detail::write(invalid, 164, inventoryIndexCrc(std::span(invalid).first(164)), 4);
    EXPECT_FALSE(decodeContentRemovalRecord(invalid, output));
    EXPECT_EQ(output, expected);
  }
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE + 2> unaligned{};
  unaligned.front() = 0xAA;
  unaligned.back() = 0xBB;
  ASSERT_EQ(encodeContentRemovalRecord(expected, std::span(unaligned).subspan(1, bytes.size())), bytes.size());
  ASSERT_TRUE(decodeContentRemovalRecord(std::span(unaligned).subspan(1, bytes.size()), output));
  EXPECT_EQ(output, expected);
  EXPECT_EQ(unaligned.front(), 0xAA);
  EXPECT_EQ(unaligned.back(), 0xBB);
}
TEST(ContentRemovalJournal, OrderedDurablePhasesAndDuplicateCommandsSurviveRestart) {
  StorageFake storage;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
  const auto expected = initial();
  ContentRemovalJournal journal(storage, scratch);
  ASSERT_EQ(journal.begin(expected), ContentRemovalJournalResult::Ok);
  EXPECT_EQ(journal.advance(ContentRemovalPhase::Committed), ContentRemovalJournalResult::Invalid);
  for (const auto phase : {ContentRemovalPhase::Prepared, ContentRemovalPhase::Quarantined,
                           ContentRemovalPhase::Committed, ContentRemovalPhase::Retired}) {
    ASSERT_EQ(journal.advance(phase), ContentRemovalJournalResult::Ok);
    const auto writes = storage.writes;
    EXPECT_EQ(journal.advance(phase), ContentRemovalJournalResult::Ok);
    EXPECT_EQ(journal.begin(expected), ContentRemovalJournalResult::Ok);
    EXPECT_EQ(storage.writes, writes);
    ContentRemovalJournal reopened(storage, scratch);
    ASSERT_EQ(reopened.recover(expected), ContentRemovalJournalResult::Ok);
    ASSERT_NE(reopened.current(), nullptr);
    EXPECT_EQ(reopened.current()->phase, phase);
  }
  EXPECT_EQ(journal.advance(ContentRemovalPhase::Prepared), ContentRemovalJournalResult::Invalid);
}
TEST(ContentRemovalJournal, InitialPersistenceFailuresRequireRecoveryBeforeProgress) {
  for (bool after : {false, true}) {
    StorageFake clean;
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
    const auto expected = initial();
    ContentRemovalJournal baseline(clean, scratch);
    ASSERT_EQ(baseline.begin(expected), ContentRemovalJournalResult::Ok);
    const auto count = clean.operations;
    for (unsigned fault = 1; fault <= count; ++fault) {
      StorageFake storage;
      storage.failAt = fault;
      storage.afterEffect = after;
      ContentRemovalJournal journal(storage, scratch);
      EXPECT_NE(journal.begin(expected), ContentRemovalJournalResult::Ok);
      EXPECT_EQ(journal.current(), nullptr);
      EXPECT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Invalid);
      storage.failAt = 0;
      ASSERT_EQ(journal.begin(expected), ContentRemovalJournalResult::Ok);
      EXPECT_EQ(journal.current()->phase, ContentRemovalPhase::Prepared);
    }
  }
}
TEST(ContentRemovalJournal, PhasePersistenceFaultsRecoverDurableState) {
  for (const auto phase :
       {ContentRemovalPhase::Quarantined, ContentRemovalPhase::Committed, ContentRemovalPhase::Retired}) {
    StorageFake seed;
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
    const auto expected = initial();
    ContentRemovalJournal setup(seed, scratch);
    ASSERT_EQ(setup.begin(expected), ContentRemovalJournalResult::Ok);
    const auto previous = static_cast<ContentRemovalPhase>(static_cast<unsigned>(phase) - 1);
    while (setup.current()->phase != previous)
      ASSERT_EQ(setup.advance(static_cast<ContentRemovalPhase>(static_cast<unsigned>(setup.current()->phase) + 1)),
                ContentRemovalJournalResult::Ok);
    for (bool after : {false, true}) {
      for (unsigned fault = 1; fault <= 3; ++fault) {
        StorageFake storage = seed;
        ContentRemovalJournal journal(storage, scratch);
        ASSERT_EQ(journal.recover(expected), ContentRemovalJournalResult::Ok);
        storage.operations = 0;
        storage.failAt = fault;
        storage.afterEffect = after;
        EXPECT_NE(journal.advance(phase), ContentRemovalJournalResult::Ok);
        EXPECT_EQ(journal.current(), nullptr);
        storage.failAt = 0;
        ContentRemovalJournal reopened(storage, scratch);
        ASSERT_EQ(reopened.recover(expected), ContentRemovalJournalResult::Ok);
        EXPECT_TRUE(reopened.current()->phase == previous || reopened.current()->phase == phase);
        EXPECT_EQ(reopened.advance(phase), ContentRemovalJournalResult::Ok);
        EXPECT_EQ(reopened.current()->phase, phase);
      }
    }
  }
}
TEST(ContentRemovalJournal, ForeignPlansCorruptionAndNonadjacentSlotsBlockRecovery) {
  const auto expected = initial();
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
  for (unsigned changed = 0; changed < 6; ++changed) {
    auto foreign = expected;
    switch (changed) {
      case 0:
        foreign.request.transaction[0] ^= 1;
        break;
      case 1:
        foreign.request.owner[0] ^= 1;
        break;
      case 2:
        foreign.request.generation[0] ^= 1;
        break;
      case 3:
        foreign.request.manifest.contentHash[0] ^= 1;
        break;
      case 4:
        ++foreign.request.manifest.length;
        break;
      case 5:
        foreign.planHash[0] ^= 1;
        break;
    }
    StorageFake storage;
    storage.files[CONTENT_REMOVAL_JOURNALS[0]] = encoded(expected);
    storage.files[CONTENT_REMOVAL_JOURNALS[1]] = encoded(foreign);
    ContentRemovalJournal journal(storage, scratch);
    EXPECT_EQ(journal.recover(expected), ContentRemovalJournalResult::Conflict);
    EXPECT_EQ(journal.current(), nullptr);
    EXPECT_EQ(storage.writes, 0);
  }
  StorageFake storage;
  storage.files[CONTENT_REMOVAL_JOURNALS[0]] = encoded(expected);
  auto later = expected;
  later.phase = ContentRemovalPhase::Committed;
  later.revision = 3;
  storage.files[CONTENT_REMOVAL_JOURNALS[1]] = encoded(later);
  ContentRemovalJournal journal(storage, scratch);
  EXPECT_EQ(journal.recover(expected), ContentRemovalJournalResult::Corrupt);
  storage.files[CONTENT_REMOVAL_JOURNALS[1]].resize(10);
  EXPECT_EQ(journal.recover(expected), ContentRemovalJournalResult::Ok);
  storage.files[CONTENT_REMOVAL_JOURNALS[0]].resize(10);
  EXPECT_EQ(journal.recover(expected), ContentRemovalJournalResult::Corrupt);
  EXPECT_EQ(journal.current(), nullptr);
}

#include "lib/Companion/CompanionContentRemoval.h"
namespace {
struct ParticipantFake final : ContentRemovalParticipant {
  ContentRemovalJournal& journal;
  bool valid = true, quarantined = false, published = false, retired = false;
  bool afterEffect = false;
  unsigned failAt = 0, operations = 0, mutations = 0, invalidateAt = 0;
  explicit ParticipantFake(ContentRemovalJournal& journal) : journal(journal) {}
  bool fail() {
    ++operations;
    if (operations == invalidateAt && journal.current()) {
      auto foreign = initial();
      foreign.request.owner[0] ^= 1;
      EXPECT_EQ(journal.recover(foreign), ContentRemovalJournalResult::Conflict);
    }
    return operations == failAt;
  }
  bool verifyPlan(const ContentRemovalRecord&) override { return !fail() && valid && !published; }
  bool quarantine(const ContentRemovalRecord&) override {
    EXPECT_NE(journal.current(), nullptr);
    EXPECT_EQ(journal.current()->phase, ContentRemovalPhase::Prepared);
    const bool failed = fail();
    if (!failed || afterEffect) {
      quarantined = true;
      ++mutations;
    }
    return !failed;
  }
  bool verifyQuarantined(const ContentRemovalRecord&) override { return !fail() && quarantined && !retired; }
  bool publishRemoval(const ContentRemovalRecord&) override {
    EXPECT_EQ(journal.current()->phase, ContentRemovalPhase::Quarantined);
    const bool failed = fail();
    if (!failed || afterEffect) {
      published = true;
      ++mutations;
    }
    return !failed;
  }
  bool verifyPublished(const ContentRemovalRecord&) override { return !fail() && quarantined && published; }
  bool retireBackups(const ContentRemovalRecord&) override {
    EXPECT_EQ(journal.current()->phase, ContentRemovalPhase::Committed);
    const bool failed = fail();
    if (!failed || afterEffect) {
      retired = true;
      ++mutations;
    }
    return !failed;
  }
  bool verifyRetired(const ContentRemovalRecord&) override { return !fail() && published && retired; }
};
}  // namespace
TEST(ContentRemovalJournal, ParticipantFailuresResumeWithoutEarlyBackupRetirement) {
  for (bool after : {false, true}) {
    for (unsigned fault = 1; fault <= 11; ++fault) {
      StorageFake storage;
      std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
      ContentRemovalJournal journal(storage, scratch);
      ParticipantFake participant(journal);
      participant.failAt = fault;
      participant.afterEffect = after;
      ContentRemoval removal(journal, participant);
      EXPECT_NE(removal.remove(initial()), ContentRemovalJournalResult::Ok);
      participant.failAt = 0;
      ASSERT_EQ(removal.remove(initial()), ContentRemovalJournalResult::Ok);
      EXPECT_TRUE(participant.retired);
      ASSERT_NE(journal.current(), nullptr);
      EXPECT_EQ(journal.current()->phase, ContentRemovalPhase::Retired);
      const auto mutations = participant.mutations;
      EXPECT_EQ(removal.remove(initial()), ContentRemovalJournalResult::Ok);
      EXPECT_EQ(participant.mutations, mutations);
    }
  }
}
TEST(ContentRemovalJournal, InvalidParticipantPlanNeverCreatesAnIntentOrMutatesContent) {
  StorageFake storage;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
  ContentRemovalJournal journal(storage, scratch);
  ParticipantFake participant(journal);
  participant.valid = false;
  ContentRemoval removal(journal, participant);
  EXPECT_EQ(removal.remove(initial()), ContentRemovalJournalResult::Conflict);
  EXPECT_TRUE(storage.files.empty());
  EXPECT_EQ(participant.mutations, 0);
}

TEST(ContentRemovalJournal, ParticipantBoundaryOwnershipChangesStopFurtherMutations) {
  for (unsigned boundary = 2; boundary <= 11; ++boundary) {
    StorageFake storage;
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
    ContentRemovalJournal journal(storage, scratch);
    ParticipantFake participant(journal);
    participant.invalidateAt = boundary;
    ContentRemoval removal(journal, participant);
    EXPECT_EQ(removal.remove(initial()), ContentRemovalJournalResult::Conflict);
    const unsigned expectedMutations = boundary < 3 ? 0 : boundary < 6 ? 1 : boundary < 9 ? 2 : 3;
    EXPECT_EQ(participant.mutations, expectedMutations);
    participant.invalidateAt = 0;
    EXPECT_EQ(removal.remove(initial()), ContentRemovalJournalResult::Ok);
  }
}
TEST(ContentRemovalJournal, SilentWriteCorruptionPreservesRecoverablePreviousPhase) {
  StorageFake storage;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
  ContentRemovalJournal journal(storage, scratch);
  ASSERT_EQ(journal.begin(initial()), ContentRemovalJournalResult::Ok);
  storage.corruptWrite = true;
  EXPECT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Corrupt);
  EXPECT_EQ(journal.current(), nullptr);
  storage.corruptWrite = false;
  ASSERT_EQ(journal.recover(initial()), ContentRemovalJournalResult::Ok);
  EXPECT_EQ(journal.current()->phase, ContentRemovalPhase::Prepared);
  EXPECT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
}

TEST(ContentRemovalJournal, RecoveredCommitMustBeSyncedBeforeBackupRetirement) {
  for (bool after : {false, true}) {
    StorageFake storage;
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
    ContentRemovalJournal journal(storage, scratch);
    ASSERT_EQ(journal.begin(initial()), ContentRemovalJournalResult::Ok);
    ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
    storage.operations = 0;
    storage.failAt = 1;
    storage.afterEffect = true;
    EXPECT_EQ(journal.advance(ContentRemovalPhase::Committed), ContentRemovalJournalResult::IoError);
    EXPECT_EQ(journal.current(), nullptr);
    ParticipantFake participant(journal);
    participant.quarantined = participant.published = true;
    ContentRemoval removal(journal, participant);
    storage.operations = 0;
    storage.failAt = 6;
    storage.afterEffect = after;
    EXPECT_EQ(removal.remove(initial()), ContentRemovalJournalResult::IoError);
    EXPECT_EQ(participant.operations, 0);
    EXPECT_EQ(participant.mutations, 0);
    EXPECT_FALSE(participant.retired);
    storage.failAt = 0;
    EXPECT_EQ(removal.remove(initial()), ContentRemovalJournalResult::Ok);
    EXPECT_TRUE(participant.retired);
  }
}

TEST(ContentRemovalJournal, BootRecoveryFindsPersistedOwnerAndChecksBothSlots) {
  const auto expected = initial();
  StorageFake storage;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
  ContentRemovalJournal journal(storage, scratch);
  EXPECT_EQ(journal.recover(expected.request.generation), ContentRemovalJournalResult::Missing);
  ASSERT_EQ(journal.begin(expected), ContentRemovalJournalResult::Ok);
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
  ContentRemovalJournal rebooted(storage, scratch);
  ASSERT_EQ(rebooted.recover(expected.request.generation), ContentRemovalJournalResult::Ok);
  ASSERT_NE(rebooted.current(), nullptr);
  EXPECT_EQ(rebooted.current()->request, expected.request);
  EXPECT_EQ(rebooted.current()->planHash, expected.planHash);
  EXPECT_EQ(rebooted.current()->phase, ContentRemovalPhase::Quarantined);
  const auto savedFiles = storage.files;
  auto wrong = expected.request.generation;
  wrong[0] ^= 1;
  EXPECT_EQ(rebooted.recover(wrong), ContentRemovalJournalResult::Conflict);
  EXPECT_EQ(rebooted.current(), nullptr);
  EXPECT_EQ(storage.files, savedFiles);
  for (unsigned field = 0; field < 5; ++field) {
    auto foreign = expected;
    switch (field) {
      case 0:
        foreign.request.owner[0] ^= 1;
        break;
      case 1:
        foreign.request.transaction[0] ^= 1;
        break;
      case 2:
        foreign.planHash[0] ^= 1;
        break;
      case 3:
        foreign.request.manifest.contentHash[0] ^= 1;
        break;
      case 4:
        ++foreign.request.manifest.length;
        break;
    }
    for (unsigned at = 0; at < 2; ++at) {
      storage.files = savedFiles;
      storage.files[CONTENT_REMOVAL_JOURNALS[at]] = encoded(foreign);
      EXPECT_EQ(rebooted.recover(expected.request.generation), ContentRemovalJournalResult::Conflict);
      EXPECT_EQ(rebooted.current(), nullptr);
    }
  }
}
TEST(ContentRemovalJournal, BootRecoveryPreservesCorruptEvidenceAndRejectsInvalidAdmission) {
  StorageFake storage;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
  ContentRemovalJournal journal(storage, scratch);
  EXPECT_EQ(journal.recover(Identity{}), ContentRemovalJournalResult::Invalid);
  EXPECT_EQ(storage.operations, 0);
  ContentRemovalJournal shortWorkspace(storage, std::span(scratch).first(scratch.size() - 1));
  EXPECT_EQ(shortWorkspace.recover(initial().request.generation), ContentRemovalJournalResult::Invalid);
  EXPECT_EQ(storage.operations, 0);
  storage.files[CONTENT_REMOVAL_JOURNALS[0]] = {1, 2, 3};
  const auto evidence = storage.files;
  EXPECT_EQ(journal.recover(initial().request.generation), ContentRemovalJournalResult::Corrupt);
  EXPECT_EQ(storage.files, evidence);
  storage.files[CONTENT_REMOVAL_JOURNALS[1]] = encoded(initial());
  EXPECT_EQ(journal.recover(initial().request.generation), ContentRemovalJournalResult::Ok);
  EXPECT_EQ(journal.current()->phase, ContentRemovalPhase::Prepared);
}

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "lib/Companion/CompanionDictionaryRemovalParticipant.h"
using namespace companion;
namespace {
struct JournalFiles final : ContentRemovalJournalStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  bool prepare() override { return true; }
  FileStatus stat(const char* path, uint64_t& size) override {
    if (!files.contains(path)) return FileStatus::Missing;
    size = files.at(path).size();
    return FileStatus::Present;
  }
  bool read(const char* path, std::span<uint8_t> output) override {
    if (!files.contains(path) || files.at(path).size() != output.size()) return false;
    std::copy(files.at(path).begin(), files.at(path).end(), output.begin());
    return true;
  }
  bool write(const char* path, std::span<const uint8_t> input) override {
    files[path] = {input.begin(), input.end()};
    return true;
  }
};
struct Proof {
  uint64_t length;
  Digest hash;
  bool operator==(const Proof&) const = default;
};
DictionaryRemovalPlan makePlan(bool synonyms) {
  DictionaryRemovalPlan p;
  p.request.transaction.fill(1);
  p.request.owner.fill(2);
  p.request.generation.fill(3);
  p.request.manifest.kind = ContentKind::Dictionary;
  p.request.manifest.formatVersion = 1;
  p.request.manifest.length = 1000;
  p.request.manifest.contentHash.fill(4);
  auto& i = p.installed;
  i.revision = 1;
  i.phase = DictionaryInstallationPhase::Committed;
  i.extraction.revision = 1;
  i.extraction.transaction = p.request.transaction;
  i.extraction.generation = p.request.generation;
  i.extraction.archiveHash = p.request.manifest.contentHash;
  i.extraction.synonyms = synonyms;
  i.extraction.compressed = true;
  i.extraction.sealed = i.published = synonyms ? 15 : 7;
  i.extraction.lengths = {100, 200, 300, synonyms ? 400U : 0U};
  for (unsigned member = 0; member < (synonyms ? 4U : 3U); ++member) i.extraction.hashes[member].fill(member + 5);
  i.archives.original = i.archives.members = p.request.manifest;
  i.archives.members.contentHash.fill(9);
  std::strcpy(i.base.data(), "/dictionaries/Family/dictionary");
  return p;
}
struct Members final : DictionaryRemovalStorage {
  DictionaryRemovalPlan expected;
  Digest digest;
  std::array<std::optional<Proof>, 4> original, saved;
  unsigned operations = 0, failAt = 0, mutations = 0, retired = 0;
  bool afterEffect = false;
  ContentRemovalJournal* invalidate = nullptr;
  ContentRemovalRecord foreign;
  explicit Members(const DictionaryRemovalPlan& p) : expected(p) {
    digest.fill(8);
    for (unsigned member = 0; member < 4; ++member)
      original[member] = Proof{p.installed.extraction.lengths[member], p.installed.extraction.hashes[member]};
  }
  bool fail() { return ++operations == failAt; }
  bool verifyPlan(const DictionaryRemovalPlan& p, const Digest& hash) override {
    return !fail() && p == expected && hash == digest;
  }
  FileStatus stat(unsigned member, bool backup) override {
    if (fail()) return FileStatus::Error;
    return (backup ? saved : original)[member] ? FileStatus::Present : FileStatus::Missing;
  }
  bool verify(unsigned member, bool backup, uint64_t length, const Digest& hash) override {
    const auto& file = (backup ? saved : original)[member];
    return !fail() && file && *file == (Proof{length, hash});
  }
  bool quarantine(unsigned member) override {
    bool failed = fail();
    if (failed && !afterEffect) return false;
    if (!original[member] || saved[member]) return false;
    saved[member] = original[member];
    original[member].reset();
    ++mutations;
    if (invalidate) {
      EXPECT_EQ(invalidate->recover(foreign), ContentRemovalJournalResult::Conflict);
    }
    return !failed;
  }
  bool remove(unsigned member, bool backup) override {
    bool failed = fail();
    if (failed && !afterEffect) return false;
    auto& file = (backup ? saved : original)[member];
    if (!file) return false;
    file.reset();
    ++mutations;
    if (backup) ++retired;
    return !failed;
  }
};
struct References final : DictionaryRemovalReferences {
  bool published = false, retired = false, failPublish = false, failRetire = false;
  Members* corruptOnRetire = nullptr;
  bool verifyPlan(const ContentRemovalRecord&, const DictionaryRemovalPlan&) override { return true; }
  bool publish(const ContentRemovalRecord&, const DictionaryRemovalPlan&) override {
    published = true;
    return !failPublish;
  }
  bool verify(const ContentRemovalRecord&, const DictionaryRemovalPlan&) override { return published; }
  bool retire(const ContentRemovalRecord&, const DictionaryRemovalPlan&) override {
    if (corruptOnRetire) corruptOnRetire->saved[2]->hash[0] ^= 1;
    retired = true;
    return !failRetire;
  }
  bool verifyRetired(const ContentRemovalRecord&, const DictionaryRemovalPlan&) override {
    return published && retired;
  }
};
ContentRemovalRecord seed(const DictionaryRemovalPlan& p, const Members& members) {
  ContentRemovalRecord r;
  r.request = p.request;
  r.planHash = members.digest;
  return r;
}
}  // namespace
TEST(DictionaryRemovalParticipant, EveryMemberOperationFaultRecoversAndRepeatedCompletionIsHarmless) {
  for (bool synonyms : {false, true}) {
    const auto plan = makePlan(synonyms);
    unsigned normalOperations = 0;
    for (bool after : {false, true}) {
      for (unsigned fault = 0; fault <= normalOperations; ++fault) {
        JournalFiles files;
        std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
        ContentRemovalJournal journal(files, scratch);
        Members members(plan);
        members.failAt = fault;
        members.afterEffect = after;
        const auto unrelated = members.original[3];
        References refs;
        DictionaryRemovalParticipant participant(journal, members, refs);
        const bool bound = participant.bind(plan, members.digest);
        const auto initial = seed(plan, members);
        if (bound) {
          ContentRemoval removal(journal, participant);
          const auto result = removal.remove(initial);
          if (!fault) {
            EXPECT_EQ(result, ContentRemovalJournalResult::Ok);
          } else {
            EXPECT_NE(result, ContentRemovalJournalResult::Ok);
          }
        } else {
          EXPECT_EQ(fault, 1U);
        }
        if (!fault) normalOperations = members.operations;
        members.failAt = 0;
        ContentRemovalJournal recovered(files, scratch);
        DictionaryRemovalParticipant resumed(recovered, members, refs);
        ASSERT_TRUE(resumed.bind(plan, members.digest));
        ContentRemoval retry(recovered, resumed);
        ASSERT_EQ(retry.remove(initial), ContentRemovalJournalResult::Ok) << fault << "/" << after;
        EXPECT_EQ(recovered.current()->phase, ContentRemovalPhase::Retired);
        for (unsigned member = 0; member < (synonyms ? 4U : 3U); ++member) {
          EXPECT_FALSE(members.original[member]);
          EXPECT_FALSE(members.saved[member]);
        }
        if (!synonyms) {
          EXPECT_EQ(members.original[3], unrelated);
        }
        const auto mutations = members.mutations;
        EXPECT_EQ(retry.remove(initial), ContentRemovalJournalResult::Ok);
        EXPECT_EQ(members.mutations, mutations);
      }
    }
  }
}
TEST(DictionaryRemovalParticipant, InvalidSourceProofRefusesAllQuarantineAndJournalCreation) {
  const auto plan = makePlan(true);
  JournalFiles files;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
  ContentRemovalJournal journal(files, scratch);
  Members members(plan);
  members.original[3]->hash[0] ^= 1;
  References refs;
  DictionaryRemovalParticipant participant(journal, members, refs);
  ASSERT_TRUE(participant.bind(plan, members.digest));
  ContentRemoval removal(journal, participant);
  EXPECT_EQ(removal.remove(seed(plan, members)), ContentRemovalJournalResult::Conflict);
  EXPECT_EQ(members.mutations, 0U);
  EXPECT_TRUE(files.files.empty());
}
TEST(DictionaryRemovalParticipant, PublicationAndRetirementFailuresKeepOwnedBackupsRecoverable) {
  for (bool retire : {false, true}) {
    const auto plan = makePlan(true);
    JournalFiles files;
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
    ContentRemovalJournal journal(files, scratch);
    Members members(plan);
    References refs;
    refs.failPublish = !retire;
    refs.failRetire = retire;
    DictionaryRemovalParticipant participant(journal, members, refs);
    ASSERT_TRUE(participant.bind(plan, members.digest));
    ContentRemoval removal(journal, participant);
    EXPECT_EQ(removal.remove(seed(plan, members)), ContentRemovalJournalResult::IoError);
    EXPECT_EQ(members.retired, 0U);
    for (const auto& backup : members.saved) EXPECT_TRUE(backup);
    refs.failPublish = refs.failRetire = false;
    ContentRemovalJournal recovered(files, scratch);
    DictionaryRemovalParticipant resumed(recovered, members, refs);
    ASSERT_TRUE(resumed.bind(plan, members.digest));
    ContentRemoval retry(recovered, resumed);
    EXPECT_EQ(retry.remove(seed(plan, members)), ContentRemovalJournalResult::Ok);
  }
}
TEST(DictionaryRemovalParticipant, CorruptLaterBackupPreventsAnyFurtherRetirement) {
  const auto plan = makePlan(true);
  JournalFiles files;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
  ContentRemovalJournal journal(files, scratch);
  Members members(plan);
  References refs;
  refs.corruptOnRetire = &members;
  DictionaryRemovalParticipant participant(journal, members, refs);
  ASSERT_TRUE(participant.bind(plan, members.digest));
  ContentRemoval removal(journal, participant);
  EXPECT_EQ(removal.remove(seed(plan, members)), ContentRemovalJournalResult::IoError);
  EXPECT_EQ(members.retired, 0U);
  for (const auto& backup : members.saved) EXPECT_TRUE(backup);
  refs.corruptOnRetire = nullptr;
  members.saved[2]->hash[0] ^= 1;
  EXPECT_EQ(removal.remove(seed(plan, members)), ContentRemovalJournalResult::Ok);
}
TEST(DictionaryRemovalParticipant, OwnershipChangeAfterRenameStopsRemainingMemberMutations) {
  const auto plan = makePlan(true);
  JournalFiles files;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> scratch{};
  ContentRemovalJournal journal(files, scratch);
  Members members(plan);
  References refs;
  auto initial = seed(plan, members);
  members.foreign = initial;
  members.foreign.request.owner.fill(7);
  members.invalidate = &journal;
  DictionaryRemovalParticipant participant(journal, members, refs);
  ASSERT_TRUE(participant.bind(plan, members.digest));
  ContentRemoval removal(journal, participant);
  EXPECT_EQ(removal.remove(initial), ContentRemovalJournalResult::IoError);
  EXPECT_EQ(members.mutations, 1U);
  EXPECT_FALSE(refs.published);
  members.invalidate = nullptr;
  ContentRemovalJournal recovered(files, scratch);
  DictionaryRemovalParticipant resumed(recovered, members, refs);
  ASSERT_TRUE(resumed.bind(plan, members.digest));
  ContentRemoval retry(recovered, resumed);
  EXPECT_EQ(retry.remove(initial), ContentRemovalJournalResult::Ok);
}

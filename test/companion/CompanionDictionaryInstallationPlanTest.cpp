#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/Companion/CompanionDictionaryInstallationJournal.h"
#include "lib/Companion/CompanionDictionaryInstallationPaths.h"
#include "lib/Companion/CompanionDictionaryInstallationPlan.h"
#include "lib/Companion/CompanionDictionaryRetirementProof.h"
using namespace companion;
namespace {
struct PlanStorage final : DictionaryExtractionJournalStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  bool failWrite = false, afterEffect = false, failRead = false, corruptWrite = false;
  unsigned writes = 0;
  bool prepare() override { return true; }
  FileStatus stat(const char* path, uint64_t& length) override {
    if (!files.contains(path)) return FileStatus::Missing;
    length = files.at(path).size();
    return FileStatus::Present;
  }
  bool read(const char* path, uint64_t at, std::span<uint8_t> bytes) override {
    if (failRead || at || !files.contains(path) || files.at(path).size() != bytes.size()) return false;
    std::copy(files.at(path).begin(), files.at(path).end(), bytes.begin());
    return true;
  }
  bool write(const char* path, uint64_t at, std::span<const uint8_t> bytes, bool truncate) override {
    ++writes;
    if (at || !truncate || (failWrite && !afterEffect)) return false;
    files[path] = {bytes.begin(), bytes.end()};
    if (corruptWrite) files[path][0] ^= 1;
    return !failWrite;
  }
};
class DictionaryInstallationPlanTest : public testing::Test {
 protected:
  DictionaryInstallationPlan plan;
  DictionaryInstallationPlanCodec codec;
  std::array<uint8_t, DICTIONARY_INSTALLATION_PLAN_SIZE> bytes{};
  void SetUp() override {
    plan.revision = 1;
    plan.extraction.revision = 4;
    plan.extraction.transaction[0] = 1;
    plan.extraction.generation[0] = 2;
    plan.extraction.archiveHash[0] = 3;
    plan.extraction.lengths = {6, 24, 111, 0};
    plan.extraction.sealed = 7;
    for (unsigned at = 0; at < 3; ++at) plan.extraction.hashes[at][0] = at + 1;
    plan.archives.original.kind = plan.archives.members.kind = ContentKind::Dictionary;
    plan.archives.original.formatVersion = plan.archives.members.formatVersion = 1;
    plan.archives.original.length = plan.archives.members.length = 1000;
    plan.archives.original.contentHash = plan.extraction.archiveHash;
    plan.archives.members.contentHash[0] = 4;
    std::strcpy(plan.base.data(), "/dictionaries/imported/dictionary");
  }
};
TEST_F(DictionaryInstallationPlanTest, RetirementProofRequiresCommittedPlanAndPreservesFailedDecodeOutput) {
  std::array<uint8_t, DICTIONARY_RETIREMENT_PROOF_SIZE> proof{};
  proof.fill(0xaa);
  const auto untouched = proof;
  EXPECT_EQ(DictionaryRetirementProofCodec::encode(plan, proof), 0u);
  EXPECT_EQ(proof, untouched);
  plan.phase = DictionaryInstallationPhase::Committed;
  plan.published = 7;
  ASSERT_EQ(DictionaryRetirementProofCodec::encode(plan, proof), proof.size());
  DictionaryRetirementProofCodec retirement;
  DictionaryInstallationPlan output;
  ASSERT_TRUE(retirement.decode(proof, output));
  EXPECT_EQ(output, plan);
  output.revision = 99;
  const auto unchanged = output;
  for (size_t at = 0; at < proof.size(); ++at) {
    proof[at] ^= 1;
    EXPECT_FALSE(retirement.decode(proof, output)) << at;
    EXPECT_EQ(output, unchanged);
    proof[at] ^= 1;
    EXPECT_FALSE(retirement.decode(std::span(proof).first(at), output)) << at;
    EXPECT_EQ(output, unchanged);
  }
  proof[5] = 1;
  inventory_detail::write(proof, proof.size() - 4, inventoryIndexCrc(std::span(proof).first(proof.size() - 4)), 4);
  EXPECT_FALSE(retirement.decode(proof, output));
  EXPECT_EQ(output, unchanged);
}
TEST_F(DictionaryInstallationPlanTest, EveryPublicationBoundaryRoundTripsOwnedPaths) {
  for (auto phase : {DictionaryInstallationPhase::Prepared, DictionaryInstallationPhase::Publishing,
                     DictionaryInstallationPhase::Bound, DictionaryInstallationPhase::Committed}) {
    plan.phase = phase;
    for (unsigned mask : {0u, 4u, 6u, 7u}) {
      plan.published = mask;
      if (!validDictionaryInstallationPlan(plan)) continue;
      ASSERT_EQ(codec.encode(plan, bytes), bytes.size());
      DictionaryInstallationPlan output;
      ASSERT_TRUE(codec.decode(bytes, output));
      EXPECT_EQ(output, plan);
      bytes.fill(0);
      EXPECT_STREQ(output.base.data(), "/dictionaries/imported/dictionary");
    }
  }
}
TEST_F(DictionaryInstallationPlanTest, MemberPathsSelectFormatAndPreserveOutputOnRejection) {
  std::array<char, DICTIONARY_INSTALLATION_MEMBER_PATH_CAPACITY> path{};
  for (unsigned member = 0; member < 3; ++member) {
    ASSERT_TRUE(dictionaryInstallationMemberPath(plan, member, path));
    static constexpr const char* EXPECTED[] = {"/dictionaries/imported/dictionary.dict",
                                               "/dictionaries/imported/dictionary.idx",
                                               "/dictionaries/imported/dictionary.ifo"};
    EXPECT_STREQ(path.data(), EXPECTED[member]);
  }
  const auto unchanged = path;
  EXPECT_FALSE(dictionaryInstallationMemberPath(plan, 3, path));
  EXPECT_FALSE(dictionaryInstallationMemberPath(plan, 4, path));
  EXPECT_FALSE(dictionaryInstallationMemberPath(plan, 0, std::span(path).first(4)));
  EXPECT_EQ(path, unchanged);
  plan.extraction.compressed = true;
  ASSERT_TRUE(dictionaryInstallationMemberPath(plan, 0, path));
  EXPECT_STREQ(path.data(), "/dictionaries/imported/dictionary.dict.dz");
  plan.extraction.synonyms = true;
  plan.extraction.sealed = 15;
  plan.extraction.lengths[3] = 12;
  plan.extraction.hashes[3][0] = 9;
  ASSERT_TRUE(dictionaryInstallationMemberPath(plan, 3, path));
  EXPECT_STREQ(path.data(), "/dictionaries/imported/dictionary.syn");
  plan.base.fill('x');
  const auto beforeInvalid = path;
  EXPECT_FALSE(dictionaryInstallationMemberPath(plan, 0, path));
  EXPECT_EQ(path, beforeInvalid);
}
TEST_F(DictionaryInstallationPlanTest, LongestMemberPathRequiresRoomForTerminator) {
  const std::string maximum = "/dictionaries/f/" + std::string(111, 'x');
  ASSERT_EQ(maximum.size(), 127u);
  std::copy(maximum.begin(), maximum.end(), plan.base.begin());
  plan.base.back() = '\0';
  plan.extraction.compressed = true;
  std::array<char, DICTIONARY_INSTALLATION_MEMBER_PATH_CAPACITY> path{};
  path.fill('!');
  const auto unchanged = path;
  EXPECT_FALSE(dictionaryInstallationMemberPath(plan, 0, std::span(path).first(135)));
  EXPECT_EQ(path, unchanged);
  ASSERT_TRUE(dictionaryInstallationMemberPath(plan, 0, path));
  EXPECT_EQ(std::string(path.data()), maximum + ".dict.dz");
  EXPECT_EQ(path.back(), '\0');
}
TEST_F(DictionaryInstallationPlanTest, EveryCorruptionAndTruncationPreservesOutput) {
  ASSERT_EQ(codec.encode(plan, bytes), bytes.size());
  auto output = plan;
  output.revision = 99;
  const auto unchanged = output;
  for (size_t at = 0; at < bytes.size(); ++at) {
    bytes[at] ^= 1;
    EXPECT_FALSE(codec.decode(bytes, output)) << at;
    EXPECT_EQ(output, unchanged);
    bytes[at] ^= 1;
    EXPECT_FALSE(codec.decode(std::span(bytes).first(at), output)) << at;
    EXPECT_EQ(output, unchanged);
  }
}
TEST_F(DictionaryInstallationPlanTest, UnsafeDestinationsIncompleteReceiptsAndMismatchedArchiveCannotEncode) {
  for (const char* path : {"/books/demo/dictionary", "/dictionaries/.hidden/dictionary", "/dictionaries/a/../b",
                           "/dictionaries/a/b/c", "/dictionaries/a/.dictionary", "/dictionaries/a/"}) {
    std::strcpy(plan.base.data(), path);
    EXPECT_EQ(codec.encode(plan, bytes), 0u) << path;
  }
  SetUp();
  plan.extraction.sealed = 6;
  plan.extraction.hashes[0] = {};
  EXPECT_EQ(codec.encode(plan, bytes), 0u);
  SetUp();
  plan.archives.original.contentHash[0] ^= 1;
  EXPECT_EQ(codec.encode(plan, bytes), 0u);
  SetUp();
  plan.published = 1;
  EXPECT_EQ(codec.encode(plan, bytes), 0u);
}
TEST_F(DictionaryInstallationPlanTest, ValidCrcCannotHideInvalidPhaseMaskPaddingOrPathLength) {
  ASSERT_EQ(codec.encode(plan, bytes), bytes.size());
  const auto good = bytes;
  auto output = plan;
  output.revision = 99;
  const auto unchanged = output;
  for (unsigned fault = 0; fault < 5; ++fault) {
    bytes = good;
    if (fault == 0) bytes[5] = 99;
    if (fault == 1) bytes[6] = 1;
    if (fault == 2) bytes[DICTIONARY_INSTALLATION_PATH_OFFSET + 127] = 'x';
    if (fault == 3) ++bytes[DICTIONARY_INSTALLATION_PATH_OFFSET - 2];
    if (fault == 4) bytes[5] = static_cast<uint8_t>(DictionaryInstallationPhase::Committed);
    inventory_detail::write(bytes, bytes.size() - 4, inventoryIndexCrc(std::span(bytes).first(bytes.size() - 4)), 4);
    EXPECT_FALSE(codec.decode(bytes, output)) << fault;
    EXPECT_EQ(output, unchanged);
  }
}
TEST_F(DictionaryInstallationPlanTest, DestinationUtf8IsStrictAndMaximumBaseFitsReader) {
  for (const char* malformed : {"\x80", "\xc0\x80", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80",
                                "\xf5\x80\x80\x80", "\xe2\x82", "\xf0\x9f"}) {
    const auto path = std::string("/dictionaries/") + malformed + "/dictionary";
    EXPECT_FALSE(validDictionaryInstallationBase(path));
    plan.base.fill(0);
    std::copy(path.begin(), path.end(), plan.base.begin());
    EXPECT_EQ(codec.encode(plan, bytes), 0u);
  }
  for (const char* folder : {"café", "cafe\xcc\x81", "\xf0\x9f\x98\x80"})
    EXPECT_TRUE(validDictionaryInstallationBase(std::string("/.dictionaries/") + folder + "/dictionary"));
  const auto maximum = std::string("/dictionaries/") + std::string(102, 'a') + "/dictionary";
  ASSERT_EQ(maximum.size(), 127u);
  EXPECT_TRUE(validDictionaryInstallationBase(maximum));
  plan.base.fill(0);
  std::copy(maximum.begin(), maximum.end(), plan.base.begin());
  ASSERT_EQ(codec.encode(plan, bytes), bytes.size());
  DictionaryInstallationPlan output;
  ASSERT_TRUE(codec.decode(bytes, output));
  EXPECT_EQ(output, plan);
  EXPECT_FALSE(validDictionaryInstallationBase(maximum + "x"));
}
TEST_F(DictionaryInstallationPlanTest, JournalPersistsOnlyForwardPublicationAndCommitTransitions) {
  PlanStorage storage;
  DictionaryInstallationJournal journal(storage, bytes);
  ASSERT_EQ(journal.begin(plan), DictionaryJournalResult::Ok);
  EXPECT_EQ(journal.markCommitted(), DictionaryJournalResult::Invalid);
  ASSERT_EQ(journal.startPublishing(), DictionaryJournalResult::Ok);
  EXPECT_EQ(journal.markBound(), DictionaryJournalResult::Invalid);
  EXPECT_EQ(journal.recordPublished(0), DictionaryJournalResult::Invalid);
  for (unsigned member : {2u, 1u, 0u}) {
    ASSERT_EQ(journal.recordPublished(member), DictionaryJournalResult::Ok);
    const auto writes = storage.writes;
    EXPECT_EQ(journal.recordPublished(member), DictionaryJournalResult::Ok);
    EXPECT_EQ(storage.writes, writes);
    DictionaryInstallationJournal recovered(storage, bytes);
    ASSERT_EQ(recovered.recover(plan), DictionaryJournalResult::Ok);
    EXPECT_EQ(*recovered.current(), *journal.current());
  }
  ASSERT_EQ(journal.markBound(), DictionaryJournalResult::Ok);
  ASSERT_EQ(journal.markCommitted(), DictionaryJournalResult::Ok);
  EXPECT_EQ(journal.markCommitted(), DictionaryJournalResult::Ok);
  EXPECT_EQ(journal.startPublishing(), DictionaryJournalResult::Invalid);
  EXPECT_EQ(storage.writes, 7u);
}
TEST_F(DictionaryInstallationPlanTest, JournalEveryTruncatedReplacementRecoversPreviousPlan) {
  PlanStorage storage;
  DictionaryInstallationJournal journal(storage, bytes);
  ASSERT_EQ(journal.begin(plan), DictionaryJournalResult::Ok);
  ASSERT_EQ(journal.startPublishing(), DictionaryJournalResult::Ok);
  const auto replacement = storage.files.at(DICTIONARY_INSTALLATION_JOURNALS[1]);
  for (size_t size = 0; size < replacement.size(); ++size) {
    storage.files[DICTIONARY_INSTALLATION_JOURNALS[1]] = {replacement.begin(), replacement.begin() + size};
    ASSERT_EQ(journal.recover(plan), DictionaryJournalResult::Ok) << size;
    EXPECT_EQ(*journal.current(), plan) << size;
  }
}
TEST_F(DictionaryInstallationPlanTest, JournalAmbiguousWriteRequiresRecoveryAndForeignDestinationIsPreserved) {
  for (bool after : {false, true}) {
    PlanStorage storage;
    DictionaryInstallationJournal journal(storage, bytes);
    ASSERT_EQ(journal.begin(plan), DictionaryJournalResult::Ok);
    storage.failWrite = true;
    storage.afterEffect = after;
    EXPECT_EQ(journal.startPublishing(), DictionaryJournalResult::IoError);
    EXPECT_EQ(journal.current(), nullptr);
    storage.failWrite = false;
    ASSERT_EQ(journal.recover(plan), DictionaryJournalResult::Ok);
    EXPECT_EQ(journal.current()->phase,
              after ? DictionaryInstallationPhase::Publishing : DictionaryInstallationPhase::Prepared);
    storage.failRead = true;
    EXPECT_EQ(journal.recover(plan), DictionaryJournalResult::IoError);
    EXPECT_EQ(journal.current(), nullptr);
  }
  PlanStorage storage;
  auto foreign = plan;
  std::strcpy(foreign.base.data(), "/dictionaries/foreignX/dictionary");
  ASSERT_EQ(codec.encode(foreign, bytes), bytes.size());
  storage.files[DICTIONARY_INSTALLATION_JOURNALS[0]] = {bytes.begin(), bytes.end()};
  const auto unchanged = storage.files;
  DictionaryInstallationJournal journal(storage, bytes);
  EXPECT_EQ(journal.recover(plan), DictionaryJournalResult::Conflict);
  EXPECT_EQ(journal.current(), nullptr);
  EXPECT_EQ(storage.files, unchanged);
}
TEST_F(DictionaryInstallationPlanTest, JournalReadbackCorruptionAndConflictingRevisionsFailClosed) {
  PlanStorage storage;
  DictionaryInstallationJournal journal(storage, bytes);
  ASSERT_EQ(journal.begin(plan), DictionaryJournalResult::Ok);
  storage.corruptWrite = true;
  EXPECT_EQ(journal.startPublishing(), DictionaryJournalResult::Corrupt);
  EXPECT_EQ(journal.current(), nullptr);
  storage.corruptWrite = false;
  ASSERT_EQ(journal.recover(plan), DictionaryJournalResult::Ok);
  EXPECT_EQ(*journal.current(), plan);
  auto conflict = plan;
  conflict.phase = DictionaryInstallationPhase::Publishing;
  ASSERT_EQ(codec.encode(conflict, bytes), bytes.size());
  storage.files[DICTIONARY_INSTALLATION_JOURNALS[1]] = {bytes.begin(), bytes.end()};
  EXPECT_EQ(journal.recover(plan), DictionaryJournalResult::Corrupt);
  EXPECT_EQ(journal.current(), nullptr);
  conflict.revision = 2;
  conflict.phase = DictionaryInstallationPhase::Bound;
  conflict.published = 7;
  ASSERT_EQ(codec.encode(conflict, bytes), bytes.size());
  storage.files[DICTIONARY_INSTALLATION_JOURNALS[1]] = {bytes.begin(), bytes.end()};
  EXPECT_EQ(journal.recover(plan), DictionaryJournalResult::Corrupt);
}
}  // namespace

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>

#include "lib/hal/HalDictionaryDestinationLookup.h"
#include "lib/hal/HalDictionaryExtractionJournalStorage.h"
#include "lib/hal/HalDictionaryExtractionRecovery.h"
#include "lib/hal/HalDictionaryMemberPublicationStorage.h"
#include "lib/hal/HalDictionaryMemberVerification.h"
#include "lib/hal/HalDictionaryStagedArchive.h"
#include "lib/hal/HalDictionaryZipExtraction.h"
#include "lib/hal/HalZipEntryStage.h"
using namespace companion;
TEST(ReaderDictionaryFolderTest, FallsBackOnlyAfterCheckedAbsence) {
  inventory_hal_test::state = {};
  auto& state = inventory_hal_test::state;
  state.directories["/"] = {{"dictionaries", true}, {".dictionaries", true}};
  state.directories["/dictionaries"] = {{"es", true}};
  state.directories["/.dictionaries"] = {{"es", true}};
  state.directories["/dictionaries/es"] = {};
  state.directories["/.dictionaries/es"] = {};
  std::array<uint32_t, 256> decoded{};
  std::array<uint32_t, 1024> normalized{};
  std::array<uint8_t, 768> wanted{}, found{};
  std::array<char, 64> output{};
  HalDictionaryDestinationLookup lookup(decoded, normalized, wanted, found);
  ASSERT_EQ(lookup.resolveReaderFolder("es", output), DictionaryDestinationPresence::Present);
  EXPECT_STREQ(output.data(), "/dictionaries/es");
  state.directoryErrorPath = "/dictionaries";
  EXPECT_EQ(lookup.resolveReaderFolder("es", output), DictionaryDestinationPresence::Error);
  EXPECT_EQ(output.front(), 0);
  state.directoryErrorPath.clear();
  state.directories["/dictionaries"] = {{"ES", true}};
  EXPECT_EQ(lookup.resolveReaderFolder("es", output), DictionaryDestinationPresence::Conflict);
  EXPECT_EQ(output.front(), 0);
  state.directories["/dictionaries"].clear();
  ASSERT_EQ(lookup.resolveReaderFolder("es", output), DictionaryDestinationPresence::Present);
  EXPECT_STREQ(output.data(), "/.dictionaries/es");
  state.failClose = true;
  EXPECT_EQ(lookup.resolveReaderFolder("es", output), DictionaryDestinationPresence::Error);
  EXPECT_EQ(output.front(), 0);
  state.failClose = false;
  EXPECT_EQ(lookup.resolveReaderFolder("../es", output), DictionaryDestinationPresence::Error);
  state.directories["/.dictionaries"] = {{"caf\xc3\xa9", true}};
  state.directories["/.dictionaries/caf\xc3\xa9"] = {};
  ASSERT_EQ(lookup.resolveReaderFolder("caf\xc3\xa9", output), DictionaryDestinationPresence::Present);
  EXPECT_STREQ(output.data(), "/.dictionaries/caf\xc3\xa9");
  state.directories["/.dictionaries"].clear();
  EXPECT_EQ(lookup.resolveReaderFolder("es", output), DictionaryDestinationPresence::Missing);
  EXPECT_EQ(output.front(), 0);
}
namespace {
struct Source : InventoryIndexStorage {
  std::vector<uint8_t> bytes;
  bool size(uint64_t& output) override {
    output = bytes.size();
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.begin() + offset, output.size(), output.begin());
    return true;
  }
};
class HalZipEntryStageTest : public testing::Test {
 protected:
  std::array<uint8_t, 64> scratch;
  HalZipEntryStage stage{scratch};
  void SetUp() override { inventory_hal_test::state = {}; }
};
TEST_F(HalZipEntryStageTest, StoredAndEmptyMembersReadBackBeforeHandoffAndSurviveDestruction) {
  Source source;
  source.bytes = {'a', 'b', 'c'};
  ZipEntryExtraction extractor(source, scratch);
  ASSERT_TRUE(extractor.extract({0, 3, 3, 0x352441c2, 0, 0}, stage));
  EXPECT_TRUE(stage.isSealed());
  EXPECT_EQ(inventory_hal_test::state.files.at(ZIP_MEMBER_CANDIDATE), source.bytes);
  stage.abort();
  EXPECT_EQ(inventory_hal_test::state.files.at(ZIP_MEMBER_CANDIDATE), source.bytes);
  EXPECT_FALSE(stage.begin(3));
  EXPECT_EQ(inventory_hal_test::state.files.at(ZIP_MEMBER_CANDIDATE), source.bytes);
  inventory_hal_test::state.files.erase(ZIP_MEMBER_CANDIDATE);
  ASSERT_TRUE(extractor.extract({0, 0, 0, 0, 0, 0}, stage));
  EXPECT_TRUE(inventory_hal_test::state.files.at(ZIP_MEMBER_CANDIDATE).empty());
  EXPECT_EQ(inventory_hal_test::state.preparations, 3u);
}
TEST_F(HalZipEntryStageTest, MemberVerificationUsesReceiptAndRetainsOneHandle) {
  Source source;
  source.bytes = {'a', 'b', 'c'};
  ZipEntryExtraction extractor(source, scratch);
  ASSERT_TRUE(stage.extractMember(extractor, {0, 3, 3, 0x352441c2, 0, 0}, HalZipEntryStage::Member::Info));
  DictionaryInstallationPlan plan;
  plan.revision = plan.extraction.revision = 1;
  plan.extraction.transaction[0] = 1;
  plan.extraction.generation[0] = 2;
  plan.extraction.archiveHash[0] = 3;
  plan.extraction.lengths = {3, 3, 3, 0};
  plan.extraction.sealed = 7;
  for (unsigned at = 0; at < 3; ++at) plan.extraction.hashes[at] = stage.contentHash();
  plan.archives.original.kind = plan.archives.members.kind = ContentKind::Dictionary;
  plan.archives.original.formatVersion = plan.archives.members.formatVersion = 1;
  plan.archives.original.length = plan.archives.members.length = 100;
  plan.archives.original.contentHash = plan.extraction.archiveHash;
  plan.archives.members.contentHash[0] = 4;
  std::strcpy(plan.base.data(), "/dictionaries/demo/dictionary");
  HalDictionaryMemberVerification verification(scratch);
  const auto prepared = inventory_hal_test::state.preparations;
  ASSERT_EQ(verification.verify(plan, 2, false), DictionaryMemberPresence::Verified);
  EXPECT_EQ(inventory_hal_test::state.preparations, prepared + 1);
  auto& bytes = inventory_hal_test::state.files.at(HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info));
  bytes[0] ^= 1;
  EXPECT_EQ(verification.verify(plan, 2, false), DictionaryMemberPresence::Conflict);
  bytes[0] ^= 1;
  inventory_hal_test::state.files["/dictionaries/demo/dictionary.ifo"] = bytes;
  EXPECT_EQ(verification.verify(plan, 2, true), DictionaryMemberPresence::Verified);
  inventory_hal_test::state.failClose = true;
  EXPECT_EQ(verification.verify(plan, 2, true), DictionaryMemberPresence::Error);
  inventory_hal_test::state.failClose = false;
  EXPECT_EQ(verification.verify(plan, 3, false), DictionaryMemberPresence::Error);
  EXPECT_EQ(inventory_hal_test::state.preparations, prepared + 1);
  std::array<uint32_t, 256> decoded{};
  std::array<uint32_t, 1024> normalized{};
  std::array<uint8_t, 768> wanted{}, found{};
  HalDictionaryDestinationLookup lookup(decoded, normalized, wanted, found);
  auto& entries = inventory_hal_test::state.directories["/dictionaries/demo"];
  EXPECT_EQ(lookup.inspect(plan, 2), DictionaryDestinationPresence::Missing);
  entries.push_back({"dictionary.ifo", false});
  EXPECT_EQ(lookup.inspect(plan, 2), DictionaryDestinationPresence::Present);
  entries.push_back({"DICTIONARY.IFO", false});
  EXPECT_EQ(lookup.inspect(plan, 2), DictionaryDestinationPresence::Conflict);
  entries.clear();
  plan.base.fill(0);
  std::strcpy(plan.base.data(), "/dictionaries/demo/caf\xc3\xa9");
  entries.push_back({"cafe\xcc\x81.ifo", false});
  EXPECT_EQ(lookup.inspect(plan, 2), DictionaryDestinationPresence::Conflict);
  entries.clear();
  entries.push_back({"CAF\xc3\x89.IFO", false});
  EXPECT_EQ(lookup.inspect(plan, 2), DictionaryDestinationPresence::Conflict);
  entries.clear();
  inventory_hal_test::state.directoryErrorPath = "/dictionaries/demo";
  EXPECT_EQ(lookup.inspect(plan, 2), DictionaryDestinationPresence::Error);
  inventory_hal_test::state.directoryErrorPath.clear();
  plan.base.fill(0);
  std::strcpy(plan.base.data(), "/dictionaries/demo/short");
  entries.push_back({"unrelated long filename.ifo", false});
  inventory_hal_test::state.aliases["/dictionaries/demo/unrelated long filename.ifo"] = "SHORT.IFO";
  EXPECT_EQ(lookup.inspect(plan, 2), DictionaryDestinationPresence::Conflict);
  inventory_hal_test::state.failShortName = true;
  EXPECT_EQ(lookup.inspect(plan, 2), DictionaryDestinationPresence::Error);
  inventory_hal_test::state.failShortName = false;
  entries.clear();
  plan.base.fill(0);
  std::strcpy(plan.base.data(), "/dictionaries/demo/dictionary");
  inventory_hal_test::state.files.erase("/dictionaries/demo/dictionary.ifo");
  inventory_hal_test::state.directories["/"] = {{"dictionaries", true}};
  inventory_hal_test::state.directories["/dictionaries"] = {{"demo", true}};
  EXPECT_EQ(lookup.inspectAncestors(plan), DictionaryDestinationPresence::Present);
  EXPECT_EQ(lookup.inspectEmptyFolder(plan), DictionaryDestinationFolder::Empty);
  entries.push_back({"unknown.bin", false});
  EXPECT_EQ(lookup.inspectEmptyFolder(plan), DictionaryDestinationFolder::Occupied);
  entries.clear();
  inventory_hal_test::state.directoryErrorPath = "/dictionaries/demo";
  EXPECT_EQ(lookup.inspectEmptyFolder(plan), DictionaryDestinationFolder::Error);
  inventory_hal_test::state.directoryErrorPath.clear();
  inventory_hal_test::state.directories["/dictionaries"].push_back({"DEMO", true});
  EXPECT_EQ(lookup.inspectAncestors(plan), DictionaryDestinationPresence::Conflict);
  EXPECT_EQ(lookup.inspectEmptyFolder(plan), DictionaryDestinationFolder::Conflict);
  inventory_hal_test::state.directories["/dictionaries"].pop_back();
  bool allowed = false;
  const auto guard = [](void* context, const DictionaryInstallationPlan&) { return *static_cast<bool*>(context); };
  HalDictionaryMemberPublicationStorage publication(lookup, verification, guard, &allowed);
  EXPECT_EQ(publication.inspect(plan, 2, false), DictionaryMemberPresence::Error);
  EXPECT_FALSE(publication.move(plan, 2));
  EXPECT_EQ(inventory_hal_test::state.renames, 0u);
  allowed = true;
  EXPECT_EQ(publication.inspect(plan, 2, false), DictionaryMemberPresence::Verified);
  EXPECT_EQ(publication.inspect(plan, 2, true), DictionaryMemberPresence::Missing);
  EXPECT_FALSE(publication.move(plan, 2));
  plan.phase = DictionaryInstallationPhase::Publishing;
  inventory_hal_test::state.failRename = 1;
  EXPECT_FALSE(publication.move(plan, 2));
  EXPECT_TRUE(inventory_hal_test::state.files.contains(HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info)));
  EXPECT_FALSE(inventory_hal_test::state.files.contains("/dictionaries/demo/dictionary.ifo"));
  inventory_hal_test::state.failRename = 0;
  ASSERT_TRUE(publication.move(plan, 2));
  EXPECT_FALSE(inventory_hal_test::state.files.contains(HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info)));
  entries.push_back({"dictionary.ifo", false});
  EXPECT_EQ(publication.inspect(plan, 2, false), DictionaryMemberPresence::Missing);
  EXPECT_EQ(publication.inspect(plan, 2, true), DictionaryMemberPresence::Verified);
  EXPECT_FALSE(publication.move(plan, 2));
  EXPECT_EQ(inventory_hal_test::state.renames, 2u);
}
TEST_F(HalZipEntryStageTest, ActualDeflateExtractionProducesVerifiedMemberBytes) {
  Source source;
  std::ifstream file(ZIP_ENTRY_FIXTURE, std::ios::binary);
  source.bytes = {std::istreambuf_iterator<char>(file), {}};
  tinfl_decompressor decoder{};
  std::array<uint8_t, 32768> window;
  ZipEntryExtraction extractor(source, scratch, &decoder, window);
  ASSERT_TRUE(extractor.extract({0, source.bytes.size(), 120000, 0x7146dd0b, 8, 0}, stage));
  const auto& bytes = inventory_hal_test::state.files.at(ZIP_MEMBER_CANDIDATE);
  ASSERT_EQ(bytes.size(), 120000u);
  for (size_t at = 0; at < bytes.size(); ++at) ASSERT_EQ(bytes[at], "abcdef"[at % 6]);
  EXPECT_TRUE(stage.isSealed());
}
TEST_F(HalZipEntryStageTest, DictionaryReceiptFailureStopsBeforeNextMemberAndRetainsSealedInfo) {
  Source source;
  source.bytes = {'a', 'b', 'c'};
  ZipEntryExtraction extractor(source, scratch);
  DictionaryZipMembers members;
  members.info = members.index = members.definitions = {0, 3, 3, 0x352441c2, 0, 0};
  unsigned calls = 0;
  auto reject = [](void* context, HalZipEntryStage::Member role, const Digest&) {
    ++*static_cast<unsigned*>(context);
    EXPECT_EQ(role, HalZipEntryStage::Member::Info);
    return false;
  };
  HalDictionaryZipExtraction extraction(extractor, stage, reject, &calls);
  EXPECT_FALSE(extraction.extract(members));
  EXPECT_FALSE(extraction.isComplete());
  EXPECT_EQ(calls, 1u);
  EXPECT_EQ(extraction.sealedMembers(), 4u);
  EXPECT_NE(extraction.contentHash(HalZipEntryStage::Member::Info), nullptr);
  EXPECT_EQ(extraction.contentHash(HalZipEntryStage::Member::Index), nullptr);
  EXPECT_EQ(inventory_hal_test::state.files.size(), 1u);
  EXPECT_FALSE(extraction.extract(members));
  EXPECT_EQ(extraction.sealedMembers(), 0u);
  EXPECT_EQ(calls, 1u);
  EXPECT_EQ(inventory_hal_test::state.files.size(), 1u);
  inventory_hal_test::state.files.clear();
  HalDictionaryZipExtraction retry(extractor, stage);
  EXPECT_TRUE(retry.extract(members));
  EXPECT_TRUE(retry.isComplete());
  EXPECT_EQ(retry.sealedMembers(), 7u);
  EXPECT_EQ(retry.contentHash(HalZipEntryStage::Member::Synonyms), nullptr);
}
TEST_F(HalZipEntryStageTest, RecoveryPreservesSealedMembersAndRemovesOnlyVerifiedPartialStages) {
  Source source;
  source.bytes = {'a', 'b', 'c'};
  ZipEntryExtraction extractor(source, scratch);
  DictionaryExtractionReceipt initial;
  initial.revision = 1;
  initial.transaction[0] = 1;
  initial.generation[0] = 2;
  initial.archiveHash[0] = 3;
  initial.lengths = {3, 3, 3, 0};
  TransferState state;
  state.transaction = initial.transaction;
  state.storageGeneration = initial.generation;
  state.owner[0] = 9;
  state.contentHash = initial.archiveHash;
  state.length = state.durableOffset = 1000;
  ContentManifest manifest;
  manifest.kind = ContentKind::Dictionary;
  manifest.formatVersion = 1;
  manifest.contentHash = initial.archiveHash;
  manifest.length = state.length;
  std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> bytes{};
  HalDictionaryExtractionJournalStorage storage;
  DictionaryExtractionJournal journal(storage, bytes);
  DictionaryExtractionParent parent(journal, state, manifest, initial.generation);
  HalDictionaryExtractionRecovery recovery(scratch);
  const auto info = HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info);
  const auto index = HalZipEntryStage::memberPath(HalZipEntryStage::Member::Index);
  inventory_hal_test::state.files[index] = {7};
  EXPECT_FALSE(recovery.begin(parent, initial));
  EXPECT_FALSE(inventory_hal_test::state.files.contains(DICTIONARY_EXTRACTION_JOURNALS[0]));
  inventory_hal_test::state.files.erase(index);
  ASSERT_TRUE(recovery.begin(parent, initial));
  ASSERT_TRUE(stage.extractMember(extractor, {0, 3, 3, 0x352441c2, 0, 0}, HalZipEntryStage::Member::Info));
  ASSERT_EQ(parent.recordSealed(2, stage.contentHash()), DictionaryJournalResult::Ok);
  inventory_hal_test::state.files[index] = {7};
  inventory_hal_test::state.directories[index] = {};
  EXPECT_FALSE(recovery.verifyAndDiscardPartial(parent));
  EXPECT_TRUE(inventory_hal_test::state.files.contains(index));
  inventory_hal_test::state.directories.erase(index);
  inventory_hal_test::state.files[index] = {7, 7, 7, 7};
  EXPECT_FALSE(recovery.verifyAndDiscardPartial(parent));
  EXPECT_TRUE(inventory_hal_test::state.files.contains(index));
  inventory_hal_test::state.files[index] = {7};
  HalDictionaryExtractionRecovery cancelled(scratch, [](void*) { return false; });
  EXPECT_FALSE(cancelled.verifyAndDiscardPartial(parent));
  EXPECT_TRUE(inventory_hal_test::state.files.contains(index));
  inventory_hal_test::state.files[info][0] ^= 1;
  EXPECT_FALSE(recovery.verifyAndDiscardPartial(parent));
  EXPECT_TRUE(inventory_hal_test::state.files.contains(index));
  inventory_hal_test::state.files[info][0] ^= 1;
  inventory_hal_test::state.failRemove = true;
  EXPECT_FALSE(recovery.verifyAndDiscardPartial(parent));
  EXPECT_TRUE(inventory_hal_test::state.files.contains(index));
  inventory_hal_test::state.failRemove = false;
  ASSERT_TRUE(recovery.verifyAndDiscardPartial(parent));
  EXPECT_FALSE(inventory_hal_test::state.files.contains(index));
  EXPECT_EQ(inventory_hal_test::state.files.at(info), source.bytes);
  EXPECT_TRUE(recovery.verifyAndDiscardPartial(parent));
  DictionaryExtractionJournal recovered(storage, bytes);
  DictionaryExtractionParent recoveredParent(recovered, state, manifest, initial.generation);
  ASSERT_EQ(recoveredParent.recover(initial), DictionaryJournalResult::Ok);
  DictionaryZipMembers members;
  members.info = members.index = members.definitions = {0, 3, 3, 0x352441c2, 0, 0};
  HalDictionaryZipExtraction resumed(extractor, stage);
  auto changed = members;
  changed.index.expandedBytes = 4;
  EXPECT_FALSE(resumed.resume(changed, recoveredParent, recovery));
  EXPECT_FALSE(resumed.isComplete());
  ASSERT_TRUE(resumed.resume(members, recoveredParent, recovery));
  EXPECT_TRUE(resumed.isComplete());
  EXPECT_EQ(resumed.sealedMembers(), 7u);
  EXPECT_EQ(recoveredParent.current()->sealed, 7u);
  EXPECT_EQ(inventory_hal_test::state.files.at(info), source.bytes);
  EXPECT_EQ(inventory_hal_test::state.files.at(index), source.bytes);
  EXPECT_TRUE(resumed.resume(members, recoveredParent, recovery));
  HalDictionaryStagedArchive canonical(scratch);
  ContentManifest output;
  output.kind = ContentKind::Font;
  const auto unchanged = output;
  EXPECT_FALSE(canonical.build(members, resumed, output));
  EXPECT_EQ(output, unchanged);
  EXPECT_EQ(canonical.current(), nullptr);
  EXPECT_FALSE(inventory_hal_test::state.files.contains(DICTIONARY_CACHE_CANDIDATE));
  EXPECT_EQ(inventory_hal_test::state.files.at(info), source.bytes);
}
TEST_F(HalZipEntryStageTest, DictionaryMembersCoexistUsingOneSinkAndPreserveExistingCandidates) {
  Source source;
  source.bytes = {'a', 'b', 'c'};
  ZipEntryExtraction extractor(source, scratch);
  const ZipEntrySpan entry{0, 3, 3, 0x352441c2, 0, 0};
  for (auto member : {HalZipEntryStage::Member::Definitions, HalZipEntryStage::Member::Index,
                      HalZipEntryStage::Member::Info, HalZipEntryStage::Member::Synonyms}) {
    ASSERT_TRUE(stage.extractMember(extractor, entry, member));
    EXPECT_TRUE(stage.isSealed());
    EXPECT_EQ(inventory_hal_test::state.files.at(HalZipEntryStage::memberPath(member)), source.bytes);
  }
  EXPECT_EQ(inventory_hal_test::state.files.size(), 4u);
  EXPECT_EQ(inventory_hal_test::state.preparations, 3u);
  EXPECT_FALSE(stage.extractMember(extractor, entry, HalZipEntryStage::Member::Info));
  EXPECT_EQ(inventory_hal_test::state.files.size(), 4u);
  stage.abort();
  EXPECT_EQ(inventory_hal_test::state.files.size(), 4u);
  EXPECT_FALSE(stage.extractMember(extractor, entry, static_cast<HalZipEntryStage::Member>(99)));
  EXPECT_FALSE(stage.isSealed());
  inventory_hal_test::state.files.erase(HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info));
  ASSERT_TRUE(stage.extractMember(extractor, entry, HalZipEntryStage::Member::Info));
  auto invalid = entry;
  invalid.offset = 99;
  EXPECT_FALSE(stage.extractMember(extractor, invalid, HalZipEntryStage::Member::Info));
  EXPECT_FALSE(stage.isSealed());
  EXPECT_EQ(inventory_hal_test::state.files.size(), 4u);
}
TEST_F(HalZipEntryStageTest, SilentCorruptionAndEveryStageFailureCannotHandOffMember) {
  for (unsigned failure = 0; failure < 8; ++failure) {
    SetUp();
    Source source;
    source.bytes = {'a', 'b', 'c'};
    ZipEntryExtraction extractor(source, scratch);
    auto& state = inventory_hal_test::state;
    if (failure == 0) state.corruptWrite = true;
    if (failure == 1) state.failWrite = true;
    if (failure == 2) state.failSync = true;
    if (failure == 3) state.failTruncate = true;
    if (failure == 4) state.readErrorPath = ZIP_MEMBER_CANDIDATE;
    if (failure == 5) state.failClosePath = ZIP_MEMBER_CANDIDATE;
    if (failure == 6) state.failOpen = true;
    if (failure == 7) state.failDirectory = true;
    EXPECT_FALSE(extractor.extract({0, 3, 3, 0x352441c2, 0, 0}, stage)) << failure;
    EXPECT_FALSE(stage.isSealed());
    state.corruptWrite = state.failWrite = state.failSync = state.failTruncate = state.failOpen = state.failDirectory =
        false;
    state.readErrorPath.clear();
    state.failClosePath.clear();
    stage.abort();
    EXPECT_FALSE(state.files.contains(ZIP_MEMBER_CANDIDATE));
  }
}
TEST_F(HalZipEntryStageTest, UnknownFilesDirectoriesAndLookupErrorsArePreserved) {
  auto& state = inventory_hal_test::state;
  state.files[ZIP_MEMBER_CANDIDATE] = {1, 2, 3};
  state.falseExists = true;
  EXPECT_FALSE(stage.begin(3));
  stage.abort();
  EXPECT_EQ(state.files.at(ZIP_MEMBER_CANDIDATE), (std::vector<uint8_t>{1, 2, 3}));
  state.files.clear();
  state.directories[ZIP_MEMBER_CANDIDATE] = {};
  EXPECT_FALSE(stage.begin(0));
  EXPECT_TRUE(state.directories.contains(ZIP_MEMBER_CANDIDATE));
  state.directories.erase(ZIP_MEMBER_CANDIDATE);
  state.directoryErrorPath = TRANSFER_DIRECTORY;
  EXPECT_FALSE(stage.begin(3));
  EXPECT_TRUE(state.files.empty());
}
TEST_F(HalZipEntryStageTest, FailedCleanupRetainsOwnershipAndCancelledOwnerCanStillAbort) {
  auto& state = inventory_hal_test::state;
  ASSERT_TRUE(stage.begin(3));
  ASSERT_TRUE(stage.write(0, std::array<uint8_t, 3>{1, 2, 3}));
  state.failRemove = true;
  stage.abort();
  EXPECT_TRUE(state.files.contains(ZIP_MEMBER_CANDIDATE));
  state.failRemove = false;
  stage.abort();
  EXPECT_FALSE(state.files.contains(ZIP_MEMBER_CANDIDATE));
  bool allowed = true;
  HalZipEntryStage cancelled(scratch, [](void* context) { return *static_cast<bool*>(context); }, &allowed);
  ASSERT_TRUE(cancelled.begin(3));
  allowed = false;
  EXPECT_FALSE(cancelled.write(0, std::array<uint8_t, 3>{1, 2, 3}));
  cancelled.abort();
  EXPECT_FALSE(state.files.contains(ZIP_MEMBER_CANDIDATE));
}
TEST_F(HalZipEntryStageTest, DeclaredLengthAndContiguousBoundsAreEnforcedBeforeHandoff) {
  EXPECT_FALSE(stage.begin(ZipEntryExtraction::MAX_EXPANDED_BYTES + 1));
  EXPECT_TRUE(inventory_hal_test::state.files.empty());
  ASSERT_TRUE(stage.begin(3));
  EXPECT_FALSE(stage.write(1, std::array<uint8_t, 1>{1}));
  stage.abort();
  EXPECT_FALSE(inventory_hal_test::state.files.contains(ZIP_MEMBER_CANDIDATE));
  ASSERT_TRUE(stage.begin(3));
  EXPECT_FALSE(stage.write(0, std::array<uint8_t, 4>{1, 2, 3, 4}));
  stage.abort();
  EXPECT_FALSE(inventory_hal_test::state.files.contains(ZIP_MEMBER_CANDIDATE));
  ASSERT_TRUE(stage.begin(3));
  ASSERT_TRUE(stage.write(0, std::array<uint8_t, 3>{1, 2, 3}));
  EXPECT_FALSE(stage.seal(2));
  EXPECT_FALSE(stage.isSealed());
  EXPECT_FALSE(inventory_hal_test::state.files.contains(ZIP_MEMBER_CANDIDATE));
}
}  // namespace

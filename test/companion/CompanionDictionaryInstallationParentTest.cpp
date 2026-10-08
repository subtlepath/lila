#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/Companion/CompanionDictionaryInstallationParent.h"
#include "lib/Companion/CompanionDictionaryMemberJournalCleanup.h"
#include "lib/Companion/CompanionDictionaryMemberPublication.h"
#include "lib/Companion/CompanionDictionaryPlanBinding.h"
#include "lib/Companion/CompanionDictionaryRetirementJournal.h"
#include "lib/Companion/CompanionDictionaryRetirementProof.h"
#include "lib/Memory/Memory.h"
#include "lib/hal/HalDictionaryCacheStorage.h"
#include "lib/hal/HalDictionaryExtractionJournalStorage.h"
#include "lib/hal/HalDictionaryInstallationReservation.h"
#include "lib/hal/HalDictionaryInstalledVerification.h"
#include "lib/hal/HalDictionaryMemberJournalCleanupStorage.h"
#include "lib/hal/HalDictionaryMemberPublicationStorage.h"
#include "lib/hal/HalDictionaryPlanBindingStorage.h"
#include "lib/hal/HalDictionaryPublicationSession.h"
#include "lib/hal/HalDictionaryRecoveredInstallation.h"
#include "lib/hal/HalDictionaryRetirementCompletionStorage.h"
#include "lib/hal/HalDictionaryRetirementOwner.h"
#include "lib/hal/HalDictionaryRetirementSession.h"
using namespace companion;
namespace {
struct PlanBindings final : DictionaryPlanBindingStorage {
  unsigned installations = 0, finalizations = 0;
  bool failInstall = false, failFinalize = false;
  bool* failReceipt = nullptr;
  bool install(const DictionaryInstallationPlan&) override {
    ++installations;
    if (failReceipt) *failReceipt = true;
    return !failInstall;
  }
  bool finalize(const DictionaryInstallationPlan&) override {
    ++finalizations;
    if (failReceipt) *failReceipt = true;
    return !failFinalize;
  }
};
struct MemberStorage final : DictionaryMemberPublicationStorage {
  std::array<DictionaryMemberPresence, 4> stages{DictionaryMemberPresence::Verified, DictionaryMemberPresence::Verified,
                                                 DictionaryMemberPresence::Verified, DictionaryMemberPresence::Missing};
  std::array<DictionaryMemberPresence, 4> targets{};
  std::vector<unsigned> moves;
  bool failMove = false, afterEffect = false;
  bool* failReceipt = nullptr;
  MemberStorage() { moves.reserve(4); }
  DictionaryMemberPresence inspect(const DictionaryInstallationPlan&, unsigned member, bool installed) override {
    return (installed ? targets : stages)[member];
  }
  bool move(const DictionaryInstallationPlan&, unsigned member) override {
    moves.push_back(member);
    if (failMove && !afterEffect) return false;
    stages[member] = DictionaryMemberPresence::Missing;
    targets[member] = DictionaryMemberPresence::Verified;
    if (failReceipt) *failReceipt = true;
    return !failMove;
  }
};
struct MemoryStorage final : TransferStorage, DictionaryMemberJournalCleanupStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  std::vector<uint8_t> payload = std::vector<uint8_t>(24, 1);
  Digest expectedHash{7};
  bool metadata = false, failRead = false, failWrite = false, failRename = false;
  bool installedProof = true;
  unsigned removals = 0, failRemoval = 0;
  bool (*publish)(void*, const ContentManifest&, const TransferState&) = nullptr;
  bool (*finalize)(void*, const ContentManifest&, const TransferState&) = nullptr;
  void* publicationContext = nullptr;
  bool verifyInstalled(const DictionaryInstallationPlan&) override { return installedProof; }
  bool prepare() override { return true; }
  FileStatus stat(const char* path, uint64_t& length) override {
    if (!files.contains(path)) return FileStatus::Missing;
    length = files.at(path).size();
    return FileStatus::Present;
  }
  bool read(const char* path, uint64_t at, std::span<uint8_t> bytes) override {
    if (failRead || !files.contains(path) || at > files.at(path).size() || bytes.size() > files.at(path).size() - at)
      return false;
    std::copy_n(files.at(path).begin() + at, bytes.size(), bytes.begin());
    return true;
  }
  bool write(const char* path, uint64_t at, std::span<const uint8_t> bytes, bool truncate) override {
    if (failWrite) return false;
    auto& file = files[path];
    if (truncate) file.clear();
    file.resize(at + bytes.size());
    std::copy(bytes.begin(), bytes.end(), file.begin() + at);
    return true;
  }
  bool resize(const char* path, uint64_t bytes) override {
    files[path].resize(bytes);
    return true;
  }
  bool rename(const char* from, const char* to) override {
    if (failRename || !files.contains(from) || files.contains(to)) return false;
    files[to] = files.at(from);
    files.erase(from);
    return true;
  }
  bool remove(const char* path) override {
    if (++removals == failRemoval) return false;
    return files.erase(path) != 0;
  }
  bool verify(const char* path, uint64_t length, const Digest& hash, std::span<uint8_t>) override {
    return files.contains(path) && files.at(path) == payload && length == payload.size() && hash == expectedHash;
  }
  bool validateContent(const char*, const char*, const ContentManifest&, std::span<uint8_t>) override { return true; }
  bool installContentMetadata(const char*, const ContentManifest&, std::span<uint8_t>) override { return metadata; }
  // This fixture models the transfer archive separately from its member store.
  bool installDictionaryMembers(const char* destination, const ContentManifest& manifest, const TransferState& state,
                                std::span<uint8_t>) override {
    if (publish) return publish(publicationContext, manifest, state);
    return !files.contains(TRANSFER_STAGE) || rename(TRANSFER_STAGE, destination);
  }
  bool finalizeContentMetadata(const char*, const ContentManifest& manifest, const TransferState& state,
                               std::span<uint8_t>) override {
    return !finalize || finalize(publicationContext, manifest, state);
  }
};
class DictionaryInstallationParentTest : public testing::Test {
 protected:
  MemoryStorage storage;
  std::array<uint8_t, TRANSFER_JOURNAL_SIZE> transferScratch{};
  std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> receiptScratch{};
  std::array<uint8_t, DICTIONARY_INSTALLATION_PLAN_SIZE> planScratch{};
  Transfer transfer{storage, transferScratch};
  DictionaryExtractionJournal receipts{storage, receiptScratch};
  DictionaryInstallationJournal plans{storage, planScratch};
  Identity generation{};
  TransferDeclaration declaration;
  DictionaryExtractionReceipt initial;
  DictionaryInstallationPlan plan;
  static constexpr const char* BASE = "/dictionaries/demo/dictionary";
  std::array<uint8_t, 64> hashScratch{};
  std::array<uint32_t, 256> decoded{};
  std::array<uint32_t, 1024> normalized{};
  std::array<uint8_t, 768> wanted{}, found{};
  HalDictionaryDestinationLookup lookup{decoded, normalized, wanted, found};
  HalDictionaryMemberVerification verification{hashScratch};
  void prepareRealMembers() {
    inventory_hal_test::state = {};
    auto& hal = inventory_hal_test::state;
    hal.directories["/"] = {{"dictionaries", true}};
    hal.directories["/dictionaries"] = {{"demo", true}};
    hal.directories["/dictionaries/demo"] = {};
    initial.lengths = {3, 3, 3, 0};
    for (const auto path : DICTIONARY_EXTRACTION_JOURNALS) storage.files.erase(path);
    DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
    ASSERT_EQ(extraction.begin(initial), DictionaryJournalResult::Ok);
    for (unsigned member : {2u, 1u, 0u}) {
      const auto path = HalZipEntryStage::memberPath(static_cast<HalZipEntryStage::Member>(member));
      hal.files[path] = {'a', 'b', 'c'};
      HalFile file;
      ASSERT_TRUE(Storage.openFileForReadReusing("COMPANION", path, file));
      Digest hash{};
      uint64_t length = 0;
      ASSERT_TRUE(hashInventoryFile(file, hashScratch, length, hash));
      ASSERT_EQ(extraction.recordSealed(member, hash), DictionaryJournalResult::Ok);
    }
    plan.extraction = *extraction.current();
  }
  void SetUp() override {
    generation[0] = 2;
    declaration.state.transaction[0] = 1;
    declaration.state.owner[0] = 9;
    declaration.state.storageGeneration = generation;
    declaration.state.contentHash[0] = 7;
    declaration.state.length = storage.payload.size();
    declaration.manifest.kind = ContentKind::Dictionary;
    declaration.manifest.formatVersion = 1;
    declaration.manifest.contentHash = declaration.state.contentHash;
    declaration.manifest.length = declaration.state.length;
    ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
    ASSERT_EQ(transfer.begin(declaration, BASE), TransferResult::Ok);
    ASSERT_EQ(transfer.append(declaration.state.transaction, declaration.state.owner, 0, storage.payload),
              TransferResult::Ok);
    initial.revision = 1;
    initial.transaction = declaration.state.transaction;
    initial.generation = generation;
    initial.archiveHash = declaration.state.contentHash;
    initial.lengths = {6, 24, 111, 0};
    DictionaryExtractionParent parent(receipts, *transfer.current(), *transfer.contentManifest(), generation);
    ASSERT_EQ(parent.begin(initial), DictionaryJournalResult::Ok);
    Digest hash{};
    hash[0] = 8;
    for (unsigned at : {2u, 1u, 0u}) ASSERT_EQ(parent.recordSealed(at, hash), DictionaryJournalResult::Ok);
    plan.revision = 1;
    plan.extraction = *parent.current();
    plan.archives.original = declaration.manifest;
    plan.archives.members = declaration.manifest;
    plan.archives.members.contentHash[0] = 8;
    std::strcpy(plan.base.data(), BASE);
  }
};
TEST_F(DictionaryInstallationParentTest, DestinationAndDurableTransferPhaseAuthorizePublication) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  auto foreign = plan;
  std::strcpy(foreign.base.data(), "/dictionaries/else/dictionary");
  const auto unchanged = storage.files;
  EXPECT_EQ(parent.begin(foreign), DictionaryJournalResult::Conflict);
  EXPECT_EQ(storage.files, unchanged);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  EXPECT_EQ(parent.startPublishing(), DictionaryJournalResult::Conflict);
  EXPECT_EQ(parent.current(), nullptr);
  ASSERT_EQ(parent.recover(plan), DictionaryJournalResult::Ok);
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  ASSERT_EQ(transfer.current()->phase, TransferPhase::Installing);
  ASSERT_EQ(parent.startPublishing(), DictionaryJournalResult::Ok);
  for (unsigned at : {2u, 1u, 0u}) ASSERT_EQ(parent.recordPublished(at), DictionaryJournalResult::Ok);
  ASSERT_EQ(parent.markBound(), DictionaryJournalResult::Ok);
  EXPECT_EQ(parent.markCommitted(), DictionaryJournalResult::Conflict);
  ASSERT_EQ(parent.recover(plan), DictionaryJournalResult::Ok);
  storage.metadata = true;
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  ASSERT_EQ(parent.markCommitted(), DictionaryJournalResult::Ok);
  EXPECT_EQ(parent.current()->phase, DictionaryInstallationPhase::Committed);
  EXPECT_EQ(transfer.destination(), BASE);
}
TEST_F(DictionaryInstallationParentTest, FailedRecoveryHidesDestinationAndPreventsPlanMutation) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  const auto unchanged = storage.files;
  EXPECT_EQ(transfer.destination(), BASE);
  storage.failRead = true;
  EXPECT_EQ(transfer.recover(generation), TransferResult::IoError);
  EXPECT_TRUE(transfer.destination().empty());
  EXPECT_EQ(parent.current(), nullptr);
  EXPECT_EQ(parent.recordPublished(2), DictionaryJournalResult::Conflict);
  EXPECT_EQ(parent.recover(plan), DictionaryJournalResult::Conflict);
  EXPECT_EQ(storage.files, unchanged);
  storage.failRead = false;
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  EXPECT_EQ(transfer.destination(), BASE);
  EXPECT_EQ(parent.recover(plan), DictionaryJournalResult::Ok);
}
TEST_F(DictionaryInstallationParentTest, AmbiguousCheckpointWriteHidesDestinationUntilRecovery) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  const auto unchanged = storage.files;
  storage.failWrite = true;
  EXPECT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  EXPECT_TRUE(transfer.destination().empty());
  EXPECT_EQ(parent.current(), nullptr);
  EXPECT_EQ(parent.startPublishing(), DictionaryJournalResult::Conflict);
  EXPECT_EQ(storage.files, unchanged);
  storage.failWrite = false;
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  EXPECT_EQ(transfer.destination(), BASE);
  EXPECT_EQ(parent.recover(plan), DictionaryJournalResult::Ok);
}
TEST_F(DictionaryInstallationParentTest, MemberPublicationRecoversRenameBeforeReceipt) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  MemberStorage members;
  DictionaryMemberPublication publication(parent, members);
  members.failMove = members.afterEffect = true;
  EXPECT_EQ(publication.publish(), DictionaryJournalResult::IoError);
  ASSERT_EQ(members.moves, std::vector<unsigned>{2});
  EXPECT_EQ(parent.current()->published, 0);
  ASSERT_EQ(parent.recover(plan), DictionaryJournalResult::Ok);
  members.failMove = false;
  EXPECT_EQ(publication.publish(), DictionaryJournalResult::Ok);
  EXPECT_EQ(members.moves, (std::vector<unsigned>{2, 1, 0}));
  EXPECT_EQ(parent.current()->published, 7);
  EXPECT_EQ(publication.publish(), DictionaryJournalResult::Ok);
  EXPECT_EQ(members.moves.size(), 3u);
}
TEST_F(DictionaryInstallationParentTest, MemberPublicationPreflightsAllFilesBeforeMutation) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  MemberStorage members;
  DictionaryMemberPublication publication(parent, members);
  const auto unchanged = storage.files;
  members.targets[0] = DictionaryMemberPresence::Conflict;
  EXPECT_EQ(publication.publish(), DictionaryJournalResult::Conflict);
  EXPECT_TRUE(members.moves.empty());
  EXPECT_EQ(storage.files, unchanged);
  members.targets[0] = DictionaryMemberPresence::Missing;
  EXPECT_EQ(publication.publish(), DictionaryJournalResult::Conflict);
  EXPECT_TRUE(members.moves.empty());
  EXPECT_EQ(storage.files, unchanged);
}
TEST_F(DictionaryInstallationParentTest, FailedReceiptAfterMoveRequiresJournalRecovery) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  MemberStorage members;
  members.failReceipt = &storage.failWrite;
  DictionaryMemberPublication publication(parent, members);
  EXPECT_EQ(publication.publish(), DictionaryJournalResult::IoError);
  EXPECT_EQ(parent.current(), nullptr);
  EXPECT_EQ(publication.publish(), DictionaryJournalResult::Invalid);
  ASSERT_EQ(members.moves, std::vector<unsigned>{2});
  members.failReceipt = nullptr;
  storage.failWrite = false;
  ASSERT_EQ(parent.recover(plan), DictionaryJournalResult::Ok);
  EXPECT_EQ(parent.current()->published, 0);
  ASSERT_EQ(publication.publish(), DictionaryJournalResult::Ok);
  EXPECT_EQ(members.moves, (std::vector<unsigned>{2, 1, 0}));
  EXPECT_EQ(parent.current()->published, 7);
}
TEST_F(DictionaryInstallationParentTest, PublicationRejectsBothLocationsAndOutOfOrderMovedMembers) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  ASSERT_EQ(parent.startPublishing(), DictionaryJournalResult::Ok);
  MemberStorage members;
  DictionaryMemberPublication publication(parent, members);
  const auto unchanged = storage.files;
  members.targets[2] = DictionaryMemberPresence::Verified;
  EXPECT_EQ(publication.publish(), DictionaryJournalResult::Conflict);
  members.targets[2] = DictionaryMemberPresence::Missing;
  members.stages[0] = DictionaryMemberPresence::Missing;
  members.targets[0] = DictionaryMemberPresence::Verified;
  EXPECT_EQ(publication.publish(), DictionaryJournalResult::Conflict);
  members.stages[0] = DictionaryMemberPresence::Error;
  EXPECT_EQ(publication.publish(), DictionaryJournalResult::IoError);
  EXPECT_TRUE(members.moves.empty());
  EXPECT_EQ(storage.files, unchanged);
}
TEST_F(DictionaryInstallationParentTest, HalReservationPersistsOnlyAfterEmptyDestinationAndRealHashes) {
  prepareRealMembers();
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  HalDictionaryInstallationReservation reservation(parent, lookup, verification);
  ASSERT_EQ(reservation.begin(plan), DictionaryJournalResult::Ok);
  ASSERT_NE(parent.current(), nullptr);
  EXPECT_EQ(*parent.current(), plan);
  const auto unchanged = storage.files;
  EXPECT_EQ(reservation.begin(plan), DictionaryJournalResult::Conflict);
  EXPECT_EQ(storage.files, unchanged);
  DictionaryInstallationJournal recovered(storage, planScratch);
  DictionaryInstallationParent recoveredParent(recovered, extraction, transfer);
  ASSERT_EQ(recoveredParent.recover(plan), DictionaryJournalResult::Ok);
  EXPECT_EQ(*recoveredParent.current(), plan);
}
TEST_F(DictionaryInstallationParentTest, HalReservationPreservesOccupiedCorruptAndForeignDestinations) {
  prepareRealMembers();
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  HalDictionaryInstallationReservation reservation(parent, lookup, verification);
  const auto unchanged = storage.files;
  auto& hal = inventory_hal_test::state;
  hal.directories["/dictionaries/demo"].push_back({"unknown.bin", false});
  EXPECT_EQ(reservation.begin(plan), DictionaryJournalResult::Conflict);
  hal.directories["/dictionaries/demo"].clear();
  auto& bytes = hal.files.at(HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info));
  bytes[0] ^= 1;
  EXPECT_EQ(reservation.begin(plan), DictionaryJournalResult::Conflict);
  bytes[0] ^= 1;
  hal.readErrorPath = HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info);
  EXPECT_EQ(reservation.begin(plan), DictionaryJournalResult::IoError);
  hal.readErrorPath.clear();
  storage.failWrite = true;
  EXPECT_EQ(reservation.begin(plan), DictionaryJournalResult::IoError);
  storage.failWrite = false;
  auto foreign = plan;
  std::strcpy(foreign.base.data(), "/dictionaries/demo/foreign");
  EXPECT_EQ(reservation.begin(foreign), DictionaryJournalResult::Conflict);
  EXPECT_EQ(storage.files, unchanged);
  EXPECT_EQ(parent.current(), nullptr);
}
TEST_F(DictionaryInstallationParentTest, HalPublicationRecoversActualMoveWithFailedAcknowledgement) {
  prepareRealMembers();
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  HalDictionaryInstallationReservation reservation(parent, lookup, verification);
  ASSERT_EQ(reservation.begin(plan), DictionaryJournalResult::Ok);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  struct Owner {
    DictionaryInstallationParent& parent;
    Transfer& transfer;
  } owner{parent, transfer};
  const auto guard = [](void* context, const DictionaryInstallationPlan& expected) {
    const auto& owner = *static_cast<Owner*>(context);
    const auto plan = owner.parent.current();
    const auto state = owner.transfer.current();
    return plan && state && *plan == expected && state->phase == TransferPhase::Installing;
  };
  auto& hal = inventory_hal_test::state;
  hal.enumerateFileMap = true;
  hal.directories[TRANSFER_DIRECTORY] = {};
  HalDictionaryMemberPublicationStorage files(lookup, verification, guard, &owner);
  DictionaryMemberPublication publication(parent, files);
  hal.failRenameAfter = 1;
  EXPECT_EQ(publication.publish(), DictionaryJournalResult::IoError);
  EXPECT_FALSE(hal.files.contains(HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info)));
  EXPECT_EQ(hal.files.at("/dictionaries/demo/dictionary.ifo"), (std::vector<uint8_t>{'a', 'b', 'c'}));
  ASSERT_NE(parent.current(), nullptr);
  EXPECT_EQ(parent.current()->published, 0);
  Transfer rebootedTransfer(storage, transferScratch);
  ASSERT_EQ(rebootedTransfer.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation),
            TransferResult::Ok);
  DictionaryExtractionJournal rebootedReceipts(storage, receiptScratch);
  DictionaryExtractionParent rebootedExtraction(rebootedReceipts, *rebootedTransfer.current(),
                                                *rebootedTransfer.contentManifest(), generation);
  ASSERT_EQ(rebootedExtraction.recover(), DictionaryJournalResult::Ok);
  DictionaryInstallationJournal rebootedPlans(storage, planScratch);
  DictionaryInstallationParent rebootedParent(rebootedPlans, rebootedExtraction, rebootedTransfer);
  ASSERT_EQ(rebootedParent.recover(plan), DictionaryJournalResult::Ok);
  Owner rebootedOwner{rebootedParent, rebootedTransfer};
  HalDictionaryMemberPublicationStorage rebootedFiles(lookup, verification, guard, &rebootedOwner);
  DictionaryMemberPublication rebootedPublication(rebootedParent, rebootedFiles);
  hal.failRenameAfter = 0;
  auto& installedInfo = hal.files.at("/dictionaries/demo/dictionary.ifo");
  installedInfo[0] ^= 1;
  const auto unchangedJournals = storage.files;
  EXPECT_EQ(rebootedPublication.publish(), DictionaryJournalResult::Conflict);
  EXPECT_EQ(hal.renames, 1u);
  EXPECT_EQ(storage.files, unchangedJournals);
  installedInfo[0] ^= 1;
  ASSERT_EQ(rebootedPublication.publish(), DictionaryJournalResult::Ok);
  EXPECT_EQ(rebootedParent.current()->published, 7);
  EXPECT_EQ(hal.renames, 3u);
  ASSERT_EQ(rebootedPublication.publish(), DictionaryJournalResult::Ok);
  EXPECT_EQ(hal.renames, 3u);
  for (unsigned member = 0; member < 3; ++member) {
    EXPECT_EQ(rebootedFiles.inspect(*rebootedParent.current(), member, false), DictionaryMemberPresence::Missing);
    EXPECT_EQ(rebootedFiles.inspect(*rebootedParent.current(), member, true), DictionaryMemberPresence::Verified);
  }
}
TEST_F(DictionaryInstallationParentTest, DeferredDictionaryRecoveryVerifiesArchiveWithoutAdvancingInstall) {
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  ASSERT_EQ(transfer.current()->phase, TransferPhase::Installing);
  storage.metadata = true;
  const auto unchanged = storage.files;
  Transfer rebooted(storage, transferScratch);
  ASSERT_EQ(rebooted.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation), TransferResult::Ok);
  EXPECT_EQ(rebooted.current()->phase, TransferPhase::Installing);
  EXPECT_EQ(rebooted.destination(), BASE);
  EXPECT_EQ(storage.files, unchanged);
  storage.files.at(BASE)[0] ^= 1;
  EXPECT_EQ(rebooted.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation),
            TransferResult::HashMismatch);
  EXPECT_TRUE(rebooted.destination().empty());
  storage.files.at(BASE)[0] ^= 1;
  ASSERT_EQ(rebooted.recover(generation), TransferResult::Ok);
  EXPECT_EQ(rebooted.current()->phase, TransferPhase::Committed);
}
TEST_F(DictionaryInstallationParentTest, DeferredRecoveryVerifiesUnmovedStageAndRejectsInvalidModes) {
  storage.failRename = true;
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  ASSERT_EQ(transfer.current()->phase, TransferPhase::Installing);
  ASSERT_TRUE(storage.files.contains(TRANSFER_STAGE));
  ASSERT_FALSE(storage.files.contains(BASE));
  const auto unchanged = storage.files;
  Transfer rebooted(storage, transferScratch);
  ASSERT_EQ(rebooted.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation), TransferResult::Ok);
  EXPECT_EQ(rebooted.destination(), BASE);
  EXPECT_EQ(rebooted.current()->phase, TransferPhase::Installing);
  EXPECT_EQ(storage.files, unchanged);
  storage.files.at(TRANSFER_STAGE)[0] ^= 1;
  EXPECT_EQ(rebooted.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation),
            TransferResult::HashMismatch);
  EXPECT_TRUE(rebooted.destination().empty());
  storage.files.at(TRANSFER_STAGE)[0] ^= 1;
  EXPECT_EQ(rebooted.recover(generation, static_cast<TransferRecoveryMode>(99)), TransferResult::Invalid);
  EXPECT_TRUE(rebooted.destination().empty());
  EXPECT_EQ(storage.files, unchanged);
  auto foreignGeneration = generation;
  foreignGeneration[0] ^= 1;
  EXPECT_EQ(rebooted.recover(foreignGeneration, TransferRecoveryMode::DeferDictionaryInstallation),
            TransferResult::WrongStorage);
  EXPECT_TRUE(rebooted.destination().empty());
  EXPECT_EQ(storage.files, unchanged);
}
TEST_F(DictionaryInstallationParentTest, DeferredCommittedDictionaryRecoveryRetainsOwnerCleanupFiles) {
  storage.metadata = true;
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  storage.files[TRANSFER_BACKUP] = {'s', 'a', 'v', 'e', 'd'};
  storage.metadata = false;
  const auto unchanged = storage.files;
  Transfer rebooted(storage, transferScratch);
  EXPECT_EQ(rebooted.recover(generation), TransferResult::IoError);
  EXPECT_TRUE(rebooted.destination().empty());
  ASSERT_EQ(rebooted.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation), TransferResult::Ok);
  EXPECT_EQ(rebooted.current()->phase, TransferPhase::Committed);
  EXPECT_EQ(rebooted.destination(), BASE);
  EXPECT_EQ(storage.files, unchanged);
  storage.files.at(BASE)[0] ^= 1;
  EXPECT_EQ(rebooted.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation),
            TransferResult::HashMismatch);
  EXPECT_TRUE(rebooted.destination().empty());
  EXPECT_TRUE(storage.files.contains(TRANSFER_BACKUP));
}
TEST_F(DictionaryInstallationParentTest, PlanBindingRequiresCompleteMembersAndDurableParentCommit) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  MemberStorage members;
  PlanBindings bindings;
  DictionaryPlanBinding binding(parent, members, bindings);
  EXPECT_EQ(binding.install(), DictionaryJournalResult::Invalid);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  ASSERT_EQ(parent.startPublishing(), DictionaryJournalResult::Ok);
  EXPECT_EQ(binding.install(), DictionaryJournalResult::Invalid);
  for (unsigned member : {2u, 1u, 0u}) ASSERT_EQ(parent.recordPublished(member), DictionaryJournalResult::Ok);
  EXPECT_EQ(binding.install(), DictionaryJournalResult::Conflict);
  EXPECT_EQ(bindings.installations, 0u);
  members.stages.fill(DictionaryMemberPresence::Missing);
  members.targets.fill(DictionaryMemberPresence::Verified);
  bindings.failInstall = true;
  EXPECT_EQ(binding.install(), DictionaryJournalResult::IoError);
  EXPECT_EQ(parent.current()->phase, DictionaryInstallationPhase::Publishing);
  bindings.failInstall = false;
  ASSERT_EQ(binding.install(), DictionaryJournalResult::Ok);
  EXPECT_EQ(parent.current()->phase, DictionaryInstallationPhase::Bound);
  EXPECT_EQ(binding.finalize(), DictionaryJournalResult::Invalid);
  EXPECT_EQ(bindings.finalizations, 0u);
  storage.metadata = true;
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  bindings.failFinalize = true;
  EXPECT_EQ(binding.finalize(), DictionaryJournalResult::IoError);
  EXPECT_EQ(parent.current()->phase, DictionaryInstallationPhase::Bound);
  bindings.failFinalize = false;
  ASSERT_EQ(binding.finalize(), DictionaryJournalResult::Ok);
  EXPECT_EQ(parent.current()->phase, DictionaryInstallationPhase::Committed);
  EXPECT_EQ(binding.install(), DictionaryJournalResult::Invalid);
  EXPECT_EQ(binding.finalize(), DictionaryJournalResult::Ok);
}
TEST_F(DictionaryInstallationParentTest, BindingEffectsBeforeFailedJournalWritesRecoverAndRetry) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  ASSERT_EQ(parent.startPublishing(), DictionaryJournalResult::Ok);
  for (unsigned member : {2u, 1u, 0u}) ASSERT_EQ(parent.recordPublished(member), DictionaryJournalResult::Ok);
  MemberStorage members;
  members.stages.fill(DictionaryMemberPresence::Missing);
  members.targets.fill(DictionaryMemberPresence::Verified);
  PlanBindings bindings;
  DictionaryPlanBinding binding(parent, members, bindings);
  const auto beforeBinding = storage.files;
  bindings.failReceipt = &storage.failWrite;
  EXPECT_EQ(binding.install(), DictionaryJournalResult::IoError);
  EXPECT_EQ(bindings.installations, 1u);
  EXPECT_EQ(parent.current(), nullptr);
  EXPECT_EQ(storage.files, beforeBinding);
  EXPECT_EQ(binding.install(), DictionaryJournalResult::Invalid);
  bindings.failReceipt = nullptr;
  storage.failWrite = false;
  ASSERT_EQ(parent.recover(plan), DictionaryJournalResult::Ok);
  EXPECT_EQ(parent.current()->phase, DictionaryInstallationPhase::Publishing);
  ASSERT_EQ(binding.install(), DictionaryJournalResult::Ok);
  EXPECT_EQ(parent.current()->phase, DictionaryInstallationPhase::Bound);
  storage.metadata = true;
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  const auto beforeCleanup = storage.files;
  bindings.failReceipt = &storage.failWrite;
  EXPECT_EQ(binding.finalize(), DictionaryJournalResult::IoError);
  EXPECT_EQ(bindings.finalizations, 1u);
  EXPECT_EQ(parent.current(), nullptr);
  EXPECT_EQ(storage.files, beforeCleanup);
  EXPECT_EQ(binding.finalize(), DictionaryJournalResult::Invalid);
  bindings.failReceipt = nullptr;
  storage.failWrite = false;
  DictionaryInstallationJournal recoveredPlans(storage, planScratch);
  DictionaryInstallationParent recoveredParent(recoveredPlans, extraction, transfer);
  ASSERT_EQ(recoveredParent.recover(plan), DictionaryJournalResult::Ok);
  EXPECT_EQ(recoveredParent.current()->phase, DictionaryInstallationPhase::Bound);
  DictionaryPlanBinding recoveredBinding(recoveredParent, members, bindings);
  ASSERT_EQ(recoveredBinding.finalize(), DictionaryJournalResult::Ok);
  EXPECT_EQ(recoveredParent.current()->phase, DictionaryInstallationPhase::Committed);
  EXPECT_EQ(bindings.finalizations, 2u);
}
TEST_F(DictionaryInstallationParentTest, HalBindingPublishesFromPlanAndRecoversCommittedFinalization) {
  const auto hashBytes = [this](const char* path, const std::vector<uint8_t>& bytes, Digest& hash) {
    inventory_hal_test::state.files[path] = bytes;
    HalFile file;
    uint64_t length = 0;
    return Storage.openFileForReadReusing("COMPANION", path, file) &&
           hashInventoryFile(file, hashScratch, length, hash) && length == bytes.size();
  };
  ASSERT_TRUE(hashBytes("/hash-original", storage.payload, storage.expectedHash));
  storage.files.clear();
  declaration.state.contentHash = declaration.manifest.contentHash = storage.expectedHash;
  ASSERT_EQ(transfer.recover(generation), TransferResult::Ok);
  ASSERT_EQ(transfer.begin(declaration, BASE), TransferResult::Ok);
  ASSERT_EQ(transfer.append(declaration.state.transaction, declaration.state.owner, 0, storage.payload),
            TransferResult::Ok);
  initial.archiveHash = storage.expectedHash;
  plan.archives.original = declaration.manifest;
  plan.archives.members = declaration.manifest;
  prepareRealMembers();
  auto& hal = inventory_hal_test::state;
  hal.directories[TRANSFER_DIRECTORY] = {};
  std::array<uint8_t, DICTIONARY_BINDING_SIZE> bindingScratch{};
  HalDictionaryCacheStorage cache;
  DictionaryCachePublication archives(cache, bindingScratch);
  std::vector<uint8_t> canonicalBytes(25, 2);
  plan.archives.members.length = canonicalBytes.size();
  ASSERT_TRUE(hashBytes(DICTIONARY_CACHE_CANDIDATE, canonicalBytes, plan.archives.members.contentHash));
  ASSERT_EQ(archives.publish(plan.archives.members), DictionaryCacheResult::Ok);
  ASSERT_TRUE(hashBytes(DICTIONARY_CACHE_CANDIDATE, storage.payload, plan.archives.original.contentHash));
  ASSERT_EQ(archives.publish(plan.archives.original), DictionaryCacheResult::Ok);
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  HalDictionaryInstallationReservation reservation(parent, lookup, verification);
  ASSERT_EQ(reservation.begin(plan), DictionaryJournalResult::Ok);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  struct Owner {
    DictionaryInstallationParent* parent;
    Transfer* transfer;
  } owner{&parent, &transfer};
  const auto guard = [](void* context, const DictionaryInstallationPlan& expected) {
    const auto& owner = *static_cast<Owner*>(context);
    const auto plan = owner.parent->current();
    const auto state = owner.transfer->current();
    return plan && state && *plan == expected &&
           (state->phase == TransferPhase::Installing || state->phase == TransferPhase::Committed);
  };
  hal.enumerateFileMap = true;
  auto session = makeUniqueNoThrow<HalDictionaryPublicationSession>(parent, lookup, verification, bindingScratch);
  ASSERT_TRUE(session);
  HalDictionaryMemberPublicationStorage files(lookup, verification, guard, &owner);
  HalDictionaryBindings records(cache, bindingScratch);
  hal.failRenameAfterSource = HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info);
  EXPECT_EQ(session->install(), DictionaryJournalResult::IoError);
  EXPECT_EQ(parent.current()->published, 0u);
  EXPECT_FALSE(hal.files.contains(HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info)));
  hal.failRenameAfterSource.clear();
  hal.failSync = true;
  EXPECT_EQ(session->install(), DictionaryJournalResult::IoError);
  hal.failSync = false;
  ASSERT_EQ(session->install(), DictionaryJournalResult::Ok);
  ASSERT_EQ(parent.current()->phase, DictionaryInstallationPhase::Bound);
  const auto boundFiles = hal.files;
  ASSERT_EQ(session->install(), DictionaryJournalResult::Ok);
  EXPECT_EQ(hal.files, boundFiles);
  storage.metadata = true;
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  Transfer rebooted(storage, transferScratch);
  storage.metadata = false;
  ASSERT_EQ(rebooted.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation), TransferResult::Ok);
  auto recoveredSession = makeUniqueNoThrow<HalDictionaryRecoveredInstallation>(
      rebooted, *rebooted.current(), *rebooted.contentManifest(), generation, storage, storage, receiptScratch,
      planScratch, lookup, verification, bindingScratch);
  ASSERT_TRUE(recoveredSession);
  ASSERT_EQ(recoveredSession->recover(), DictionaryJournalResult::Ok);
  ASSERT_NE(recoveredSession->installationParent(), nullptr);
  auto& recoveredParent = *recoveredSession->installationParent();
  owner = {&recoveredParent, &rebooted};
  ASSERT_EQ(archives.find(plan.archives.members), DictionaryCacheResult::Ok);
  auto& cached = hal.files.at(archives.publishedPath());
  cached[0] ^= 1;
  EXPECT_EQ(recoveredSession->finalize(), DictionaryJournalResult::IoError);
  EXPECT_EQ(recoveredParent.current()->phase, DictionaryInstallationPhase::Bound);
  cached[0] ^= 1;
  ASSERT_EQ(recoveredSession->finalize(), DictionaryJournalResult::Ok);
  EXPECT_TRUE(recoveredSession->verifyInstalled());
  EXPECT_EQ(recoveredParent.current()->phase, DictionaryInstallationPhase::Committed);
  DictionaryArchiveBinding result;
  ASSERT_EQ(records.read(BASE, result), DictionaryBindingResult::Found);
  EXPECT_EQ(result, plan.archives);
  HalDictionaryInstalledVerification installed(files, records, guard, &owner);
  const auto committed = *recoveredParent.current();
  const auto installedFiles = hal.files;
  ASSERT_TRUE(installed.verify(committed));
  EXPECT_EQ(hal.files, installedFiles);
  hal.files.at("/dictionaries/demo/dictionary.ifo")[0] ^= 1;
  const auto damagedMember = hal.files;
  EXPECT_FALSE(installed.verify(committed));
  EXPECT_FALSE(recoveredSession->verifyInstalled());
  EXPECT_EQ(hal.files, damagedMember);
  hal.files = installedFiles;
  ASSERT_EQ(archives.find(plan.archives.original), DictionaryCacheResult::Ok);
  hal.files.at(archives.publishedPath())[0] ^= 1;
  const auto damagedArchive = hal.files;
  EXPECT_FALSE(installed.verify(committed));
  EXPECT_EQ(hal.files, damagedArchive);
  hal.files = installedFiles;
  auto foreignProof = committed;
  foreignProof.extraction.transaction[0] ^= 1;
  EXPECT_FALSE(installed.verify(foreignProof));
  EXPECT_EQ(hal.files, installedFiles);
  const auto durableJournals = storage.files;
  storage.failRead = true;
  EXPECT_EQ(recoveredSession->recover(), DictionaryJournalResult::IoError);
  EXPECT_EQ(recoveredSession->installationParent(), nullptr);
  storage.failRead = false;
  ASSERT_EQ(recoveredSession->recover(), DictionaryJournalResult::Ok);
  EXPECT_EQ(storage.files, durableJournals);
  auto copiedState = *rebooted.current();
  auto copiedOwner = makeUniqueNoThrow<HalDictionaryRecoveredInstallation>(
      rebooted, copiedState, *rebooted.contentManifest(), generation, storage, storage, receiptScratch, planScratch,
      lookup, verification, bindingScratch);
  ASSERT_TRUE(copiedOwner);
  storage.failRead = true;
  EXPECT_EQ(copiedOwner->recover(), DictionaryJournalResult::Conflict);
  EXPECT_EQ(copiedOwner->installationParent(), nullptr);
  storage.failRead = false;
  copiedOwner.reset();
  EXPECT_EQ(storage.files, durableJournals);
  ASSERT_EQ(recoveredSession->recover(), DictionaryJournalResult::Ok);
  for (const auto path : DICTIONARY_EXTRACTION_JOURNALS) hal.files[path] = storage.files.at(path);
  for (const auto path : DICTIONARY_INSTALLATION_JOURNALS) hal.files[path] = storage.files.at(path);
  std::array<uint8_t, DICTIONARY_RETIREMENT_PROOF_SIZE> proofScratch{};
  std::array<uint8_t, DICTIONARY_RETIREMENT_PROOF_SIZE> retirementWireScratch{};
  HalDictionaryExtractionJournalStorage proofStorage(HalDictionaryExtractionJournalStorage::Purpose::Retirement);
  DictionaryRetirementJournal retirement(proofStorage, proofScratch, rebooted);
  HalDictionaryExtractionJournalStorage extractionStorage;
  HalDictionaryExtractionJournalStorage installationStorage(
      HalDictionaryExtractionJournalStorage::Purpose::Installation);
  HalCompanionFileLookup initialRetirementLookup;
  for (auto invalidWire : {std::span<uint8_t>(bindingScratch), std::span<uint8_t>(proofScratch)}) {
    auto invalidOwner = makeUniqueNoThrow<HalDictionaryRetirementOwner>(
        rebooted, extractionStorage, installationStorage, proofStorage, initialRetirementLookup, lookup, verification,
        proofScratch, invalidWire);
    ASSERT_TRUE(invalidOwner);
    const auto unchanged = hal.files;
    EXPECT_EQ(invalidOwner->begin(recoveredParent), DictionaryJournalResult::Invalid);
    EXPECT_EQ(invalidOwner->resume(), DictionaryJournalResult::Invalid);
    EXPECT_EQ(hal.files, unchanged);
  }
  auto initialRetirement = makeUniqueNoThrow<HalDictionaryRetirementOwner>(
      rebooted, extractionStorage, installationStorage, proofStorage, initialRetirementLookup, lookup, verification,
      proofScratch, retirementWireScratch);
  ASSERT_TRUE(initialRetirement);
  hal.files.at("/dictionaries/demo/dictionary.ifo")[0] ^= 1;
  const auto damagedBeforeRetirement = hal.files;
  EXPECT_EQ(initialRetirement->begin(recoveredParent), DictionaryJournalResult::Conflict);
  EXPECT_EQ(hal.files, damagedBeforeRetirement);
  EXPECT_FALSE(hal.files.contains(DICTIONARY_RETIREMENT_JOURNALS[0]));
  hal.files.at("/dictionaries/demo/dictionary.ifo")[0] ^= 1;
  ASSERT_EQ(initialRetirement->begin(recoveredParent), DictionaryJournalResult::Ok);
  initialRetirement.reset();
  const Transfer* retirementOwner = &rebooted;
  const auto retirementGuard = [](void* context, const DictionaryInstallationPlan& expected) {
    const auto transfer = *static_cast<const Transfer**>(context);
    return transfer && dictionaryRetirementProofMatchesTransfer(expected, *transfer);
  };
  HalDictionaryMemberPublicationStorage retiredMembers(lookup, verification, retirementGuard, &retirementOwner);
  HalDictionaryInstalledVerification retiredInstalled(retiredMembers, records, retirementGuard, &retirementOwner);
  const auto verifyRetirement = [](void* context, const DictionaryInstallationPlan& expected) {
    return static_cast<HalDictionaryInstalledVerification*>(context)->verify(expected);
  };
  const auto survivingProof = hal.files.at(DICTIONARY_RETIREMENT_JOURNALS[0]);
  hal.files.at(DICTIONARY_RETIREMENT_JOURNALS[1]).resize(10);
  hal.failSync = true;
  EXPECT_EQ(retirement.repair(committed, verifyRetirement, &retiredInstalled), DictionaryJournalResult::IoError);
  EXPECT_EQ(retirement.current(), nullptr);
  EXPECT_EQ(hal.files.at(DICTIONARY_RETIREMENT_JOURNALS[0]), survivingProof);
  hal.failSync = false;
  ASSERT_EQ(retirement.repair(committed, verifyRetirement, &retiredInstalled), DictionaryJournalResult::Ok);
  EXPECT_TRUE(retirement.redundant());
  EXPECT_EQ(hal.files.at(DICTIONARY_RETIREMENT_JOURNALS[0]), survivingProof);
  HalDictionaryMemberJournalCleanupStorage cleanupStorage(retirement, retiredInstalled, extractionStorage,
                                                          installationStorage, retirementGuard, &retirementOwner);
  DictionaryMemberJournalCleanup cleanup(retirement, cleanupStorage, proofScratch);
  const auto beforeRemoval = hal.files;
  EXPECT_FALSE(cleanupStorage.remove(DICTIONARY_RETIREMENT_JOURNALS[0]));
  EXPECT_FALSE(cleanupStorage.write(DICTIONARY_EXTRACTION_JOURNALS[0], 0, receiptScratch, true));
  EXPECT_EQ(hal.files, beforeRemoval);
  hal.failRemoveAfter = true;
  EXPECT_EQ(cleanup.run(), DictionaryJournalResult::IoError);
  EXPECT_FALSE(hal.files.contains(DICTIONARY_EXTRACTION_JOURNALS[0]));
  EXPECT_TRUE(hal.files.contains(DICTIONARY_RETIREMENT_JOURNALS[0]));
  EXPECT_TRUE(hal.files.contains(DICTIONARY_RETIREMENT_JOURNALS[1]));
  hal.failRemoveAfter = false;
  ASSERT_EQ(cleanup.run(), DictionaryJournalResult::Ok);
  for (const auto path : DICTIONARY_EXTRACTION_JOURNALS) EXPECT_FALSE(hal.files.contains(path));
  for (const auto path : DICTIONARY_INSTALLATION_JOURNALS) EXPECT_FALSE(hal.files.contains(path));
  const auto afterRemoval = hal.files;
  ASSERT_EQ(cleanup.run(), DictionaryJournalResult::Ok);
  EXPECT_FALSE(cleanupStorage.remove(DICTIONARY_RETIREMENT_JOURNALS[0]));
  EXPECT_EQ(hal.files, afterRemoval);
  Transfer retiredTransfer(storage, transferScratch);
  ASSERT_EQ(retiredTransfer.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation), TransferResult::Ok);
  retirementOwner = &retiredTransfer;
  DictionaryRetirementJournal recoveredRetirement(proofStorage, proofScratch, retiredTransfer);
  ASSERT_EQ(recoveredRetirement.recover(), DictionaryJournalResult::Ok);
  ASSERT_NE(recoveredRetirement.current(), nullptr);
  EXPECT_EQ(*recoveredRetirement.current(), committed);
  HalDictionaryMemberJournalCleanupStorage recoveredCleanupStorage(
      recoveredRetirement, retiredInstalled, extractionStorage, installationStorage, retirementGuard, &retirementOwner);
  DictionaryMemberJournalCleanup recoveredCleanup(recoveredRetirement, recoveredCleanupStorage, proofScratch);
  ASSERT_EQ(recoveredCleanup.run(), DictionaryJournalResult::Ok);
  EXPECT_EQ(hal.files, afterRemoval);
  HalCompanionFileLookup completionLookup;
  HalDictionaryRetirementCompletionStorage completionStorage(recoveredRetirement, retiredInstalled, proofStorage,
                                                             completionLookup, retirementGuard, &retirementOwner);
  DictionaryRetirementCompletion completion(recoveredRetirement, completionStorage, proofScratch);
  EXPECT_FALSE(completionStorage.remove(DICTIONARY_CACHE_CANDIDATE));
  hal.files["/.crosspoint/companion/zip-ranges-next"] = {1};
  const auto remainingStage = hal.files;
  EXPECT_EQ(completion.run(), DictionaryJournalResult::IoError);
  EXPECT_EQ(hal.files, remainingStage);
  hal.files.erase("/.crosspoint/companion/zip-ranges-next");
  hal.files[DICTIONARY_INSTALLATION_JOURNALS[0]] = {1};
  const auto remainingJournal = hal.files;
  EXPECT_EQ(completion.run(), DictionaryJournalResult::IoError);
  EXPECT_EQ(hal.files, remainingJournal);
  hal.files.erase(DICTIONARY_INSTALLATION_JOURNALS[0]);
  hal.files.at(DICTIONARY_RETIREMENT_JOURNALS[1]).resize(10);
  const auto tornCompletion = hal.files;
  EXPECT_EQ(completion.run(), DictionaryJournalResult::Corrupt);
  EXPECT_EQ(hal.files, tornCompletion);
  hal.files = afterRemoval;
  auto retirementSession = makeUniqueNoThrow<HalDictionaryRetirementOwner>(
      retiredTransfer, extractionStorage, installationStorage, proofStorage, completionLookup, lookup, verification,
      proofScratch, retirementWireScratch);
  ASSERT_TRUE(retirementSession);
  hal.files[TRANSFER_STAGE] = storage.payload;
  hal.files.at(TRANSFER_STAGE)[0] ^= 1;
  const auto wrongIncoming = hal.files;
  EXPECT_EQ(retirementSession->resume(), DictionaryJournalResult::Corrupt);
  EXPECT_EQ(hal.files, wrongIncoming);
  hal.files.at(TRANSFER_STAGE)[0] ^= 1;
  hal.readErrorPath = TRANSFER_STAGE;
  const auto unreadableIncoming = hal.files;
  EXPECT_EQ(retirementSession->resume(), DictionaryJournalResult::IoError);
  EXPECT_EQ(hal.files, unreadableIncoming);
  hal.readErrorPath.clear();
  hal.files[TRANSFER_BACKUP] = {1};
  const auto unexpectedBackup = hal.files;
  EXPECT_EQ(retirementSession->resume(), DictionaryJournalResult::Conflict);
  EXPECT_EQ(hal.files, unexpectedBackup);
  hal.files.erase(TRANSFER_BACKUP);
  hal.failRemoveAfter = true;
  EXPECT_EQ(retirementSession->resume(), DictionaryJournalResult::IoError);
  EXPECT_FALSE(hal.files.contains(TRANSFER_STAGE));
  EXPECT_TRUE(hal.files.contains(DICTIONARY_RETIREMENT_JOURNALS[0]));
  EXPECT_TRUE(hal.files.contains(DICTIONARY_RETIREMENT_JOURNALS[1]));
  hal.failRemoveAfter = false;
  hal.files = afterRemoval;
  hal.files.erase(DICTIONARY_RETIREMENT_JOURNALS[1]);
  hal.files.at("/dictionaries/demo/dictionary.ifo")[0] ^= 1;
  const auto badRepair = hal.files;
  EXPECT_EQ(retirementSession->resume(), DictionaryJournalResult::Conflict);
  EXPECT_EQ(hal.files, badRepair);
  hal.files = afterRemoval;
  hal.files.erase(DICTIONARY_RETIREMENT_JOURNALS[1]);
  hal.failSyncPath = DICTIONARY_RETIREMENT_JOURNALS[1];
  EXPECT_EQ(retirementSession->resume(), DictionaryJournalResult::IoError);
  EXPECT_EQ(hal.files.at(DICTIONARY_RETIREMENT_JOURNALS[0]), afterRemoval.at(DICTIONARY_RETIREMENT_JOURNALS[0]));
  hal.failSyncPath.clear();
  hal.files = afterRemoval;
  hal.files["/.crosspoint/companion/zip-ranges-next"] = {1};
  const auto sessionStage = hal.files;
  EXPECT_EQ(retirementSession->resume(), DictionaryJournalResult::IoError);
  EXPECT_EQ(hal.files, sessionStage);
  hal.files = afterRemoval;
  hal.failRemoveAfter = true;
  EXPECT_EQ(retirementSession->resume(), DictionaryJournalResult::IoError);
  EXPECT_FALSE(hal.files.contains(DICTIONARY_RETIREMENT_JOURNALS[0]));
  EXPECT_TRUE(hal.files.contains(DICTIONARY_RETIREMENT_JOURNALS[1]));
  hal.failRemoveAfter = false;
  retirementSession.reset();
  retirementSession = makeUniqueNoThrow<HalDictionaryRetirementOwner>(
      retiredTransfer, extractionStorage, installationStorage, proofStorage, completionLookup, lookup, verification,
      proofScratch, retirementWireScratch);
  ASSERT_TRUE(retirementSession);
  ASSERT_EQ(retirementSession->resume(), DictionaryJournalResult::Ok);
  EXPECT_FALSE(hal.files.contains(DICTIONARY_RETIREMENT_JOURNALS[1]));
  EXPECT_EQ(retirementSession->current(), nullptr);
  auto expectedCompletion = afterRemoval;
  for (const auto path : DICTIONARY_RETIREMENT_JOURNALS) expectedCompletion.erase(path);
  EXPECT_EQ(hal.files, expectedCompletion);
  DictionaryRetirementJournal absentProof(proofStorage, proofScratch, retiredTransfer);
  EXPECT_EQ(absentProof.recover(), DictionaryJournalResult::Missing);
  EXPECT_EQ(absentProof.current(), nullptr);
  EXPECT_EQ(retirementSession->resume(), DictionaryJournalResult::Missing);
}
TEST_F(DictionaryInstallationParentTest, RetirementProofRemainsAuthorizedWithoutRemovedMemberJournals) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  DictionaryInstallationPlan proof;
  proof.revision = 99;
  const auto unchanged = proof;
  EXPECT_FALSE(dictionaryRetirementProofFromParent(parent, proof));
  EXPECT_EQ(proof, unchanged);
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  ASSERT_EQ(parent.startPublishing(), DictionaryJournalResult::Ok);
  for (unsigned member : {2u, 1u, 0u}) ASSERT_EQ(parent.recordPublished(member), DictionaryJournalResult::Ok);
  ASSERT_EQ(parent.markBound(), DictionaryJournalResult::Ok);
  storage.metadata = true;
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::Ok);
  EXPECT_FALSE(dictionaryRetirementProofFromParent(parent, proof));
  ASSERT_EQ(parent.markCommitted(), DictionaryJournalResult::Ok);
  ASSERT_TRUE(dictionaryRetirementProofFromParent(parent, proof));
  ASSERT_TRUE(dictionaryRetirementProofMatchesTransfer(proof, transfer));
  std::array<uint8_t, DICTIONARY_RETIREMENT_PROOF_SIZE> proofScratch{};
  DictionaryRetirementJournal retirement(storage, proofScratch, transfer);
  const auto beforeProof = storage.files;
  storage.failWrite = true;
  EXPECT_EQ(retirement.begin(parent), DictionaryJournalResult::IoError);
  EXPECT_EQ(retirement.current(), nullptr);
  EXPECT_EQ(storage.files, beforeProof);
  storage.failWrite = false;
  ASSERT_EQ(retirement.begin(parent), DictionaryJournalResult::Ok);
  ASSERT_TRUE(retirement.redundant());
  const auto durable = storage.files;
  ASSERT_EQ(retirement.publish(proof), DictionaryJournalResult::Ok);
  EXPECT_EQ(storage.files, durable);
  storage.files.at(DICTIONARY_RETIREMENT_JOURNALS[1]).resize(10);
  DictionaryRetirementJournal recoveredProof(storage, proofScratch, transfer);
  ASSERT_EQ(recoveredProof.recover(proof), DictionaryJournalResult::Ok);
  EXPECT_FALSE(recoveredProof.redundant());
  ASSERT_EQ(recoveredProof.recover(), DictionaryJournalResult::Ok);
  EXPECT_FALSE(recoveredProof.redundant());
  EXPECT_EQ(*recoveredProof.current(), proof);
  const auto torn = storage.files;
  EXPECT_EQ(recoveredProof.publish(proof), DictionaryJournalResult::Corrupt);
  EXPECT_EQ(storage.files, torn);
  bool installedVerified = false;
  const auto verifyRepair = [](void* context, const DictionaryInstallationPlan&) {
    return *static_cast<bool*>(context);
  };
  EXPECT_EQ(recoveredProof.repair(proof, verifyRepair, &installedVerified), DictionaryJournalResult::Conflict);
  EXPECT_EQ(storage.files, torn);
  installedVerified = true;
  storage.failWrite = true;
  EXPECT_EQ(recoveredProof.repair(proof, verifyRepair, &installedVerified), DictionaryJournalResult::IoError);
  EXPECT_EQ(storage.files, torn);
  EXPECT_EQ(recoveredProof.current(), nullptr);
  storage.failWrite = false;
  ASSERT_EQ(recoveredProof.repair(proof, verifyRepair, &installedVerified), DictionaryJournalResult::Ok);
  EXPECT_TRUE(recoveredProof.redundant());
  EXPECT_EQ(storage.files, durable);
  storage.files.at(DICTIONARY_RETIREMENT_JOURNALS[0]).resize(10);
  ASSERT_EQ(recoveredProof.recover(), DictionaryJournalResult::Ok);
  EXPECT_EQ(*recoveredProof.current(), proof);
  EXPECT_FALSE(recoveredProof.redundant());
  storage.files.at(DICTIONARY_RETIREMENT_JOURNALS[1]).resize(10);
  const auto bothTorn = storage.files;
  EXPECT_EQ(recoveredProof.repair(proof, verifyRepair, &installedVerified), DictionaryJournalResult::Corrupt);
  EXPECT_EQ(storage.files, bothTorn);
  EXPECT_EQ(recoveredProof.recover(), DictionaryJournalResult::Corrupt);
  EXPECT_EQ(recoveredProof.current(), nullptr);
  EXPECT_EQ(storage.files, bothTorn);
  storage.files = durable;
  auto foreignProof = proof;
  foreignProof.base.fill(0);
  std::strcpy(foreignProof.base.data(), "/dictionaries/demo/foreign");
  ASSERT_EQ(DictionaryRetirementProofCodec::encode(foreignProof, proofScratch), proofScratch.size());
  storage.files[DICTIONARY_RETIREMENT_JOURNALS[1]] = {proofScratch.begin(), proofScratch.end()};
  const auto foreignFiles = storage.files;
  EXPECT_EQ(recoveredProof.recover(proof), DictionaryJournalResult::Conflict);
  EXPECT_EQ(recoveredProof.current(), nullptr);
  EXPECT_EQ(storage.files, foreignFiles);
  EXPECT_EQ(recoveredProof.recover(), DictionaryJournalResult::Conflict);
  EXPECT_EQ(recoveredProof.current(), nullptr);
  EXPECT_EQ(storage.files, foreignFiles);
  EXPECT_EQ(recoveredProof.repair(proof, verifyRepair, &installedVerified), DictionaryJournalResult::Conflict);
  EXPECT_EQ(storage.files, foreignFiles);
  storage.files = durable;
  storage.failRead = true;
  EXPECT_EQ(recoveredProof.recover(), DictionaryJournalResult::IoError);
  EXPECT_EQ(recoveredProof.current(), nullptr);
  EXPECT_EQ(storage.files, durable);
  EXPECT_EQ(recoveredProof.recover(proof), DictionaryJournalResult::IoError);
  EXPECT_EQ(recoveredProof.current(), nullptr);
  EXPECT_EQ(storage.files, durable);
  storage.failRead = false;
  inventory_hal_test::state = {};
  HalDictionaryExtractionJournalStorage halProofStorage(HalDictionaryExtractionJournalStorage::Purpose::Retirement);
  DictionaryRetirementJournal halProof(halProofStorage, proofScratch, transfer);
  inventory_hal_test::state.failSync = true;
  EXPECT_EQ(halProof.begin(parent), DictionaryJournalResult::IoError);
  EXPECT_EQ(halProof.current(), nullptr);
  inventory_hal_test::state.failSync = false;
  ASSERT_EQ(halProof.begin(parent), DictionaryJournalResult::Ok);
  EXPECT_TRUE(halProof.redundant());
  EXPECT_EQ(inventory_hal_test::state.preparations, 3u);
  const auto halFiles = inventory_hal_test::state.files;
  ASSERT_EQ(DictionaryRetirementProofCodec::encode(proof, proofScratch), proofScratch.size());
  EXPECT_FALSE(halProofStorage.write(DICTIONARY_EXTRACTION_JOURNALS[0], 0, proofScratch, true));
  EXPECT_FALSE(halProofStorage.write(DICTIONARY_INSTALLATION_JOURNALS[0], 0, proofScratch, true));
  EXPECT_EQ(inventory_hal_test::state.files, halFiles);
  DictionaryRetirementJournal recoveredHalProof(halProofStorage, proofScratch, transfer);
  ASSERT_EQ(recoveredHalProof.recover(proof), DictionaryJournalResult::Ok);
  EXPECT_TRUE(recoveredHalProof.redundant());
  EXPECT_EQ(inventory_hal_test::state.preparations, 3u);
  DictionaryMemberJournalCleanup cleanup(retirement, storage, proofScratch);
  const auto beforeCleanup = storage.files;
  storage.installedProof = false;
  EXPECT_EQ(cleanup.run(), DictionaryJournalResult::IoError);
  EXPECT_EQ(storage.files, beforeCleanup);
  storage.installedProof = true;
  storage.files[DICTIONARY_INSTALLATION_JOURNALS[1]].pop_back();
  const auto malformedCleanup = storage.files;
  EXPECT_EQ(cleanup.run(), DictionaryJournalResult::Corrupt);
  EXPECT_EQ(storage.files, malformedCleanup);
  storage.files = beforeCleanup;
  auto foreignReceipt = proof.extraction;
  foreignReceipt.transaction[0] ^= 2;
  auto receiptBytes = std::span<uint8_t>(proofScratch).first(DICTIONARY_EXTRACTION_RECEIPT_SIZE);
  ASSERT_EQ(encodeDictionaryExtractionReceipt(foreignReceipt, receiptBytes), receiptBytes.size());
  storage.files[DICTIONARY_EXTRACTION_JOURNALS[0]] = {receiptBytes.begin(), receiptBytes.end()};
  const auto foreignCleanup = storage.files;
  EXPECT_EQ(cleanup.run(), DictionaryJournalResult::Conflict);
  EXPECT_EQ(storage.files, foreignCleanup);
  storage.files = beforeCleanup;
  storage.failRemoval = storage.removals + 2;
  EXPECT_EQ(cleanup.run(), DictionaryJournalResult::IoError);
  EXPECT_TRUE(storage.files.contains(DICTIONARY_RETIREMENT_JOURNALS[0]));
  EXPECT_TRUE(storage.files.contains(DICTIONARY_RETIREMENT_JOURNALS[1]));
  storage.failRemoval = 0;
  ASSERT_EQ(cleanup.run(), DictionaryJournalResult::Ok);
  for (const auto path : DICTIONARY_EXTRACTION_JOURNALS) EXPECT_FALSE(storage.files.contains(path));
  for (const auto path : DICTIONARY_INSTALLATION_JOURNALS) EXPECT_FALSE(storage.files.contains(path));
  const auto cleaned = storage.files;
  ASSERT_EQ(cleanup.run(), DictionaryJournalResult::Ok);
  EXPECT_EQ(storage.files, cleaned);
  Transfer rebooted(storage, transferScratch);
  ASSERT_EQ(rebooted.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation), TransferResult::Ok);
  EXPECT_TRUE(dictionaryRetirementProofMatchesTransfer(proof, rebooted));
  DictionaryRetirementJournal rebootedProof(storage, proofScratch, rebooted);
  ASSERT_EQ(rebootedProof.recover(proof), DictionaryJournalResult::Ok);
  EXPECT_TRUE(rebootedProof.redundant());
  auto foreign = proof;
  std::strcpy(foreign.base.data(), "/dictionaries/demo/foreign");
  EXPECT_FALSE(dictionaryRetirementProofMatchesTransfer(foreign, rebooted));
  storage.failRead = true;
  EXPECT_EQ(rebooted.recover(generation, TransferRecoveryMode::DeferDictionaryInstallation), TransferResult::IoError);
  EXPECT_FALSE(dictionaryRetirementProofMatchesTransfer(proof, rebooted));
  EXPECT_EQ(rebootedProof.recover(), DictionaryJournalResult::Invalid);
  EXPECT_EQ(rebootedProof.current(), nullptr);
}
}  // namespace

TEST_F(DictionaryInstallationParentTest, AutomaticRecoveryAuthorizesMemberPublicationWithoutEnablingCommands) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  storage.failRename = true;
  ASSERT_EQ(transfer.commit(declaration.state.transaction, declaration.state.owner), TransferResult::IoError);
  storage.failRename = false;
  Transfer reboot(storage, transferScratch);
  MemberStorage members;
  PlanBindings bindings;
  struct Owner {
    Transfer& transfer;
    DictionaryExtractionJournal& receipts;
    DictionaryInstallationJournal& plans;
    const DictionaryInstallationPlan& expected;
    MemberStorage& members;
    PlanBindings& bindings;
    bool ran = false;
  } owner{reboot, receipts, plans, plan, members, bindings};
  storage.publicationContext = &owner;
  storage.publish = [](void* context, const ContentManifest& manifest, const TransferState& state) {
    auto& owner = *static_cast<Owner*>(context);
    owner.ran = true;
    EXPECT_EQ(owner.transfer.destination(), std::string_view(owner.expected.base.data()));
    EXPECT_EQ(owner.transfer.commit(state.transaction, state.owner), TransferResult::NoTransaction);
    EXPECT_EQ(owner.transfer.abort(state.transaction, state.owner), TransferResult::NoTransaction);
    EXPECT_EQ(owner.transfer.append(state.transaction, state.owner, state.length, {}), TransferResult::NoTransaction);
    auto receiving = state;
    receiving.phase = TransferPhase::Receiving;
    receiving.durableOffset = 0;
    EXPECT_EQ(owner.transfer.begin(receiving, owner.expected.base.data()), TransferResult::Invalid);
    DictionaryExtractionParent extraction(owner.receipts, state, manifest, state.storageGeneration);
    if (extraction.recover(owner.expected.extraction) != DictionaryJournalResult::Ok) return false;
    DictionaryInstallationParent parent(owner.plans, extraction, owner.transfer);
    if (parent.recover() != DictionaryJournalResult::Ok) return false;
    DictionaryMemberPublication publication(parent, owner.members);
    DictionaryPlanBinding binding(parent, owner.members, owner.bindings);
    if (state.phase == TransferPhase::Committed) return binding.finalize() == DictionaryJournalResult::Ok;
    if (parent.current()->phase != DictionaryInstallationPhase::Bound &&
        publication.publish() != DictionaryJournalResult::Ok)
      return false;
    return binding.install() == DictionaryJournalResult::Ok;
  };
  storage.finalize = storage.publish;
  auto foreign = generation;
  foreign[0] ^= 1;
  EXPECT_EQ(reboot.recover(foreign), TransferResult::WrongStorage);
  EXPECT_FALSE(owner.ran);
  EXPECT_TRUE(reboot.destination().empty());
  ASSERT_EQ(reboot.recover(generation), TransferResult::IoError);
  EXPECT_TRUE(owner.ran);
  EXPECT_TRUE(reboot.destination().empty());
  EXPECT_EQ(reboot.commit(declaration.state.transaction, declaration.state.owner), TransferResult::NoTransaction);
  EXPECT_EQ(plans.current()->phase, DictionaryInstallationPhase::Bound);
  storage.metadata = true;
  ASSERT_EQ(reboot.recover(generation), TransferResult::Ok);
  EXPECT_TRUE(owner.ran);
  EXPECT_EQ(members.moves, (std::vector<unsigned>{2, 1, 0}));
  EXPECT_EQ(plans.current()->phase, DictionaryInstallationPhase::Committed);
  EXPECT_EQ(reboot.current()->phase, TransferPhase::Committed);
  EXPECT_EQ(bindings.installations, 2u);
  EXPECT_EQ(bindings.finalizations, 1u);
  storage.publish = nullptr;
  storage.finalize = nullptr;
}

TEST_F(DictionaryInstallationParentTest, DiskPlanRecoveryBindsTransferAndExtractionWithoutCallerPlan) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  const auto saved = storage.files;
  DictionaryInstallationJournal rebootedPlans(storage, planScratch);
  DictionaryInstallationParent rebooted(rebootedPlans, extraction, transfer);
  ASSERT_EQ(rebooted.recover(), DictionaryJournalResult::Ok);
  ASSERT_NE(rebooted.current(), nullptr);
  EXPECT_EQ(*rebooted.current(), plan);
  EXPECT_EQ(storage.files, saved);
  for (unsigned field = 0; field < 5; ++field) {
    auto foreign = plan;
    switch (field) {
      case 0:
        std::strcpy(foreign.base.data(), "/dictionaries/foreign/dictionary");
        break;
      case 1:
        foreign.extraction.transaction[0] ^= 2;
        break;
      case 2:
        foreign.extraction.generation[0] ^= 1;
        break;
      case 3:
        ++foreign.archives.original.length;
        break;
      case 4:
        foreign.extraction.hashes[0][0] ^= 1;
        break;
    }
    ASSERT_EQ(DictionaryInstallationPlanCodec::encode(foreign, planScratch), planScratch.size());
    storage.files[DICTIONARY_INSTALLATION_JOURNALS[0]] = {planScratch.begin(), planScratch.end()};
    const auto before = storage.files;
    EXPECT_EQ(rebooted.recover(), DictionaryJournalResult::Conflict);
    EXPECT_EQ(rebooted.current(), nullptr);
    EXPECT_EQ(storage.files, before);
    storage.files = saved;
  }
  storage.failRead = true;
  EXPECT_EQ(rebooted.recover(), DictionaryJournalResult::IoError);
  EXPECT_EQ(rebooted.current(), nullptr);
  storage.failRead = false;
  ASSERT_EQ(rebooted.recover(), DictionaryJournalResult::Ok);
  storage.files.clear();
  EXPECT_EQ(rebooted.recover(), DictionaryJournalResult::Missing);
  EXPECT_EQ(rebooted.current(), nullptr);
}

TEST_F(DictionaryInstallationParentTest, DiskPlanRecoveryChecksBothSlotsAndAcceptsTornInactiveSlot) {
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  ASSERT_EQ(parent.begin(plan), DictionaryJournalResult::Ok);
  DictionaryInstallationJournal rebootedPlans(storage, planScratch);
  DictionaryInstallationParent rebooted(rebootedPlans, extraction, transfer);
  auto conflicting = plan;
  conflicting.archives.members.contentHash[0] ^= 1;
  ASSERT_EQ(DictionaryInstallationPlanCodec::encode(conflicting, planScratch), planScratch.size());
  storage.files[DICTIONARY_INSTALLATION_JOURNALS[1]] = {planScratch.begin(), planScratch.end()};
  const auto saved = storage.files;
  EXPECT_EQ(rebooted.recover(), DictionaryJournalResult::Conflict);
  EXPECT_EQ(rebooted.current(), nullptr);
  EXPECT_EQ(storage.files, saved);
  storage.files.at(DICTIONARY_INSTALLATION_JOURNALS[1]).resize(3);
  ASSERT_EQ(rebooted.recover(), DictionaryJournalResult::Ok);
  EXPECT_EQ(*rebooted.current(), plan);
  storage.files.at(DICTIONARY_INSTALLATION_JOURNALS[0]).resize(2);
  EXPECT_EQ(rebooted.recover(), DictionaryJournalResult::Corrupt);
  EXPECT_EQ(rebooted.current(), nullptr);
}

TEST_F(DictionaryInstallationParentTest, ReservationCreatesMissingDirectoriesAndRejectsAliasesOrMkdirFailure) {
  prepareRealMembers();
  auto& hal = inventory_hal_test::state;
  hal.enumerateFileMap = true;
  hal.directories.erase("/dictionaries/demo");
  hal.directories.erase("/dictionaries");
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  HalDictionaryInstallationReservation reservation(parent, lookup, verification);
  hal.directories["/Dictionaries"] = {};
  const auto saved = storage.files;
  EXPECT_EQ(reservation.begin(plan), DictionaryJournalResult::Conflict);
  EXPECT_FALSE(hal.directories.contains("/dictionaries"));
  EXPECT_EQ(storage.files, saved);
  hal.directories.erase("/Dictionaries");
  auto foreign = plan;
  std::strcpy(foreign.base.data(), "/dictionaries/foreign/dictionary");
  EXPECT_EQ(reservation.begin(foreign), DictionaryJournalResult::Conflict);
  EXPECT_FALSE(hal.directories.contains("/dictionaries"));
  EXPECT_EQ(storage.files, saved);
  hal.failDirectory = true;
  EXPECT_EQ(reservation.begin(plan), DictionaryJournalResult::IoError);
  EXPECT_FALSE(hal.directories.contains("/dictionaries"));
  EXPECT_EQ(storage.files, saved);
  hal.failDirectory = false;
  ASSERT_EQ(reservation.begin(plan), DictionaryJournalResult::Ok);
  EXPECT_TRUE(hal.directories.contains("/dictionaries"));
  EXPECT_TRUE(hal.directories.contains("/dictionaries/demo"));
  ASSERT_NE(parent.current(), nullptr);
  EXPECT_EQ(*parent.current(), plan);
}

TEST_F(DictionaryInstallationParentTest, ReservationRetryRechecksPreparedProofWithoutRewritingJournal) {
  prepareRealMembers();
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  HalDictionaryInstallationReservation reservation(parent, lookup, verification);
  ASSERT_EQ(reservation.reserve(plan), DictionaryJournalResult::Ok);
  const auto saved = storage.files;
  DictionaryInstallationJournal reopenedPlans(storage, planScratch);
  DictionaryInstallationParent reopened(reopenedPlans, extraction, transfer);
  HalDictionaryInstallationReservation retry(reopened, lookup, verification);
  ASSERT_EQ(retry.reserve(plan), DictionaryJournalResult::Ok);
  EXPECT_EQ(storage.files, saved);
  const auto path = HalZipEntryStage::memberPath(HalZipEntryStage::Member::Index);
  inventory_hal_test::state.files.at(path)[0] ^= 1;
  EXPECT_EQ(retry.reserve(plan), DictionaryJournalResult::Conflict);
  EXPECT_EQ(storage.files, saved);
  inventory_hal_test::state.files.at(path)[0] ^= 1;
  ASSERT_EQ(retry.reserve(plan), DictionaryJournalResult::Ok);
  auto foreign = plan;
  std::strcpy(foreign.base.data(), "/dictionaries/foreign/dictionary");
  EXPECT_EQ(retry.reserve(foreign), DictionaryJournalResult::Conflict);
  EXPECT_EQ(storage.files, saved);
}

TEST_F(DictionaryInstallationParentTest, ReservationRetriesFailedPlanWriteWithoutChangingMemberStages) {
  prepareRealMembers();
  DictionaryExtractionParent extraction(receipts, *transfer.current(), *transfer.contentManifest(), generation);
  DictionaryInstallationParent parent(plans, extraction, transfer);
  HalDictionaryInstallationReservation reservation(parent, lookup, verification);
  const auto before = storage.files;
  const auto members = inventory_hal_test::state.files;
  storage.failWrite = true;
  EXPECT_EQ(reservation.reserve(plan), DictionaryJournalResult::IoError);
  EXPECT_EQ(parent.current(), nullptr);
  EXPECT_EQ(storage.files, before);
  EXPECT_EQ(inventory_hal_test::state.files, members);
  storage.failWrite = false;
  ASSERT_EQ(reservation.reserve(plan), DictionaryJournalResult::Ok);
  ASSERT_NE(parent.current(), nullptr);
  EXPECT_EQ(*parent.current(), plan);
  EXPECT_EQ(inventory_hal_test::state.files, members);
}

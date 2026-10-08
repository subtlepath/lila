#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>

#include "lib/Companion/CompanionDictionaryExtractionParent.h"
#include "lib/Companion/CompanionDictionaryMembersValidation.h"
#include "lib/Companion/CompanionDictionaryZipSelection.h"
#include "lib/Companion/CompanionZipArchiveValidation.h"
#include "lib/Companion/CompanionZipNormalizedNamesValidation.h"
#include "lib/Memory/Memory.h"
#include "lib/hal/HalDictionaryArchiveSelection.h"
#include "lib/hal/HalDictionaryBindingInstallation.h"
#include "lib/hal/HalDictionaryCacheStorage.h"
#include "lib/hal/HalDictionaryExtractionJournalStorage.h"
#include "lib/hal/HalDictionaryExtractionRecovery.h"
#include "lib/hal/HalDictionaryIncomingExtraction.h"
#include "lib/hal/HalDictionaryInstallationPlanBuilder.h"
#include "lib/hal/HalDictionaryInstallationPreparation.h"
#include "lib/hal/HalDictionaryOriginalArchive.h"
#include "lib/hal/HalDictionaryStagedArchive.h"
#include "lib/hal/HalDictionaryStagedSource.h"
#include "lib/hal/HalDictionaryZipExtraction.h"
#include "lib/hal/HalInventoryFileView.h"
#include "lib/hal/HalZipEntryStage.h"
#include "lib/hal/HalZipNameBytesStorage.h"
#include "lib/hal/HalZipRangeStorage.h"
using namespace companion;
namespace {
class HalZipNormalizedTest : public testing::Test {
 protected:
  std::array<uint8_t, 64> scratch{};
  std::array<uint8_t, 384> compare{};
  std::array<uint32_t, ZipNameWorkspace::WINDOW_SCALARS> windowStorage{};
  ZipNameWorkspace workspace;
  std::span<uint8_t> window;
  tinfl_decompressor decoder{};
  static DictionaryInstallationPlan initialPlan() {
    DictionaryInstallationPlan plan;
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
    return plan;
  }
  void SetUp() override {
    inventory_hal_test::state = {};
    ASSERT_TRUE(ZipNameWorkspace::borrowDecoderWindow(windowStorage, workspace, window));
  }
};
TEST_F(HalZipNormalizedTest, NfcDuplicatesFailWithUnchangedResultAndUniqueArchiveRetries) {
  HalZipRangeStorage rangesStorage, index(HalZipRangeStorage::Purpose::NameIndex);
  HalZipNameBytesStorage nameBytes;
  ZipRangeValidation ranges(rangesStorage);
  ZipNameDuplicateValidation duplicates(index, nameBytes, compare);
  HalFile file;
  HalInventoryFileView source;
  ZipNormalizedNamesValidation names(source, duplicates, workspace);
  ZipArchiveValidation archive(source, scratch, &decoder, window);
  for (const char* fixture : {"ZipNames-duplicate.fixture", "ZipNames-unique.fixture", "DictionaryBundle-plain.fixture",
                              "ZipEntry-zip64-streamed.fixture", "ZipNames-deflated.fixture"}) {
    std::ifstream input(std::string(ZIP_NORMALIZED_DIR) + "/" + fixture, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    ASSERT_FALSE(bytes.empty());
    inventory_hal_test::state.files["/source.zip"] = bytes;
    ASSERT_TRUE(Storage.openFileForReadReusing("TEST", "/source.zip", file));
    ASSERT_TRUE(source.attach(file));
    ZipDirectoryLayout result;
    result.entries = 99;
    if (std::string(fixture) == "ZipNames-duplicate.fixture") {
      EXPECT_FALSE(archive.validate(result, &ranges, &names));
      EXPECT_EQ(result.entries, 99u);
      EXPECT_FALSE(names.finish());
    } else {
      EXPECT_TRUE(archive.validate(result, &ranges, &names));
      EXPECT_EQ(result.archiveBytes, bytes.size());
    }
    EXPECT_EQ(inventory_hal_test::state.files.at("/source.zip"), bytes);
    ASSERT_TRUE(rangesStorage.discard());
    ASSERT_TRUE(index.discard());
    ASSERT_TRUE(nameBytes.discard());
  }
  EXPECT_EQ(inventory_hal_test::state.preparations, 10u);
}
TEST_F(HalZipNormalizedTest, EveryComposedCancellationPointFailsClosedAndAllowsCleanupRetry) {
  std::ifstream input(std::string(ZIP_NORMALIZED_DIR) + "/ZipNames-unique.fixture", std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  ASSERT_FALSE(bytes.empty());
  inventory_hal_test::state.files["/source.zip"] = bytes;
  HalFile file;
  ASSERT_TRUE(Storage.openFileForReadReusing("TEST", "/source.zip", file));
  HalInventoryFileView source;
  ASSERT_TRUE(source.attach(file));
  struct Control {
    unsigned calls = 0, fail = 0;
  } control;
  auto tick = [](void* ptr) {
    auto& c = *static_cast<Control*>(ptr);
    return ++c.calls != c.fail;
  };
  HalZipRangeStorage rangesStorage, index(HalZipRangeStorage::Purpose::NameIndex);
  HalZipNameBytesStorage nameBytes;
  ZipRangeValidation ranges(rangesStorage, tick, &control);
  ZipNameDuplicateValidation duplicates(index, nameBytes, compare, tick, &control);
  ZipNormalizedNamesValidation names(source, duplicates, workspace, tick, &control);
  ZipArchiveValidation archive(source, scratch, &decoder, window, tick, &control);
  ZipDirectoryLayout result;
  ASSERT_TRUE(archive.validate(result, &ranges, &names));
  const auto calls = control.calls;
  ASSERT_TRUE(rangesStorage.discard());
  ASSERT_TRUE(index.discard());
  ASSERT_TRUE(nameBytes.discard());
  for (unsigned fail = 1; fail <= calls; ++fail) {
    control.calls = 0;
    control.fail = fail;
    result.entries = 99;
    EXPECT_FALSE(archive.validate(result, &ranges, &names)) << fail;
    EXPECT_EQ(result.entries, 99u);
    EXPECT_FALSE(names.finish());
    EXPECT_FALSE(ranges.finish());
    ASSERT_TRUE(rangesStorage.discard());
    ASSERT_TRUE(index.discard());
    ASSERT_TRUE(nameBytes.discard());
  }
  control.fail = 0;
  EXPECT_TRUE(archive.validate(result, &ranges, &names));
  EXPECT_EQ(inventory_hal_test::state.files.at("/source.zip"), bytes);
}

TEST_F(HalZipNormalizedTest, WorkspaceViewsAreDisjointAndCapacityFailurePreservesOutputs) {
  EXPECT_EQ(window.size(), 32768u);
  EXPECT_EQ(workspace.raw.size(), 1024u);
  EXPECT_EQ(workspace.decoded.size(), 3072u);
  EXPECT_EQ(workspace.output.size(), 3072u);
  EXPECT_EQ(workspace.inputScalars.size(), 1024u);
  EXPECT_EQ(workspace.normalizedScalars.size(), 4096u);
  EXPECT_EQ(reinterpret_cast<uint8_t*>(workspace.inputScalars.data()), window.data() + 7168);
  EXPECT_EQ(reinterpret_cast<uint8_t*>(workspace.normalizedScalars.data()), window.data() + 11264);
  const auto originalRaw = workspace.raw.data(), originalWindow = window.data();
  EXPECT_FALSE(ZipNameWorkspace::borrowDecoderWindow(std::span(windowStorage).first(8191), workspace, window));
  EXPECT_EQ(workspace.raw.data(), originalRaw);
  EXPECT_EQ(window.data(), originalWindow);
}

TEST_F(HalZipNormalizedTest, DictionarySelectionUsesTwoValidatedNamePassesAndVisitorFailureAborts) {
  for (const char* fixture : {"DictionaryBundle-plain.fixture", "DictionaryBundle-dictzip.fixture"}) {
    for (unsigned stop : {0u, 1u, 2u, 3u, 4u}) {
      SetUp();
      std::ifstream input(std::string(ZIP_NORMALIZED_DIR) + "/" + fixture, std::ios::binary);
      std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
      ASSERT_FALSE(bytes.empty());
      ASSERT_GE(bytes.size(), 22u);
      ASSERT_EQ(bytes[bytes.size() - 2], 0u);
      ASSERT_EQ(bytes.back(), 0u);
      bytes[bytes.size() - 2] = 3;
      bytes.insert(bytes.end(), {'a', 'p', 'p'});
      inventory_hal_test::state.files["/source.zip"] = bytes;
      HalFile file;
      ASSERT_TRUE(Storage.openFileForReadReusing("TEST", "/source.zip", file));
      HalInventoryFileView source;
      ASSERT_TRUE(source.attach(file));
      HalZipRangeStorage rangeStorage, index(HalZipRangeStorage::Purpose::NameIndex);
      HalZipNameBytesStorage nameBytes;
      ZipRangeValidation ranges(rangeStorage);
      ZipNameDuplicateValidation duplicates(index, nameBytes, compare);
      DictionaryZipSelection selection;
      auto visit = [](void* context, std::span<const uint8_t> name, const ZipEntryMetadata& entry) {
        return static_cast<DictionaryZipSelection*>(context)->add(name, entry);
      };
      ZipNormalizedNamesValidation names(source, duplicates, workspace, nullptr, nullptr, visit, &selection);
      ZipArchiveValidation archive(source, scratch, &decoder, window);
      ZipDirectoryLayout layout;
      selection.begin();
      ASSERT_TRUE(archive.validate(layout, &ranges, &names));
      ASSERT_TRUE(selection.finishHeaders());
      ASSERT_TRUE(archive.validate(layout, &ranges, &names));
      DictionaryZipMembers members;
      ASSERT_TRUE(selection.finishMembers(members));
      EXPECT_TRUE(members.hasSynonyms);
      EXPECT_EQ(members.compressed, std::string(fixture) == "DictionaryBundle-dictzip.fixture");
      EXPECT_GT(members.info.expandedBytes, 0u);
      EXPECT_GT(members.index.expandedBytes, 0u);
      HalZipEntryStage stage(compare);
      ZipEntryExtraction extractor(source, scratch, &decoder, window);
      std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> receiptScratch{};
      DictionaryExtractionReceipt initial;
      initial.revision = 1;
      initial.transaction[0] = 1;
      initial.generation[0] = 2;
      uint64_t archiveBytes = 0;
      ASSERT_TRUE(hashInventoryFile(file, compare, archiveBytes, initial.archiveHash));
      initial.lengths = {members.definitions.expandedBytes, members.index.expandedBytes, members.info.expandedBytes,
                         members.synonyms.expandedBytes};
      initial.compressed = members.compressed;
      initial.synonyms = members.hasSynonyms;
      HalDictionaryExtractionJournalStorage receiptStorage;
      DictionaryExtractionJournal journal(receiptStorage, receiptScratch);
      TransferState parentState;
      parentState.transaction = initial.transaction;
      parentState.storageGeneration = initial.generation;
      parentState.owner[0] = 9;
      parentState.contentHash = initial.archiveHash;
      parentState.length = parentState.durableOffset = archiveBytes;
      ContentManifest manifest;
      manifest.kind = ContentKind::Dictionary;
      manifest.formatVersion = 1;
      manifest.length = archiveBytes;
      manifest.contentHash = initial.archiveHash;
      DictionaryExtractionParent authorized(journal, parentState, manifest, initial.generation);
      HalDictionaryExtractionRecovery recovery(compare);
      ASSERT_TRUE(recovery.begin(authorized, initial));
      struct ReceiptControl {
        DictionaryExtractionParent* parent;
        unsigned stop, calls = 0;
      } control{&authorized, stop};
      auto persistReceipt = [](void* context, HalZipEntryStage::Member member, const Digest& hash) {
        auto& control = *static_cast<ReceiptControl*>(context);
        if (control.parent->recordSealed(static_cast<unsigned>(member), hash) != DictionaryJournalResult::Ok)
          return false;
        return ++control.calls != control.stop;
      };
      HalDictionaryZipExtraction extraction(extractor, stage, persistReceipt, &control);
      EXPECT_EQ(extraction.extract(members), stop == 0);
      EXPECT_EQ(extraction.isComplete(), stop == 0);
      DictionaryExtractionJournal recovered(receiptStorage, receiptScratch);
      DictionaryExtractionParent recoveredParent(recovered, parentState, manifest, initial.generation);
      ASSERT_EQ(recoveredParent.recover(initial), DictionaryJournalResult::Ok);
      ASSERT_NE(recoveredParent.current(), nullptr);
      static constexpr std::array<uint8_t, 5> EXPECTED_MASKS{15, 4, 6, 7, 15};
      EXPECT_EQ(recoveredParent.current()->sealed, EXPECTED_MASKS[stop]);
      const auto callbacks = control.calls;
      ASSERT_TRUE(extraction.resume(members, recoveredParent, recovery));
      EXPECT_EQ(control.calls, callbacks);
      EXPECT_TRUE(extraction.isComplete());
      EXPECT_EQ(extraction.sealedMembers(), 15u);
      EXPECT_EQ(recoveredParent.current()->sealed, 15u);
      for (unsigned at = 0; at < 4; ++at)
        EXPECT_EQ(recoveredParent.current()->hashes[at],
                  *extraction.contentHash(static_cast<HalZipEntryStage::Member>(at)));
      HalDictionaryStagedSource stagedSource;
      ASSERT_TRUE(stagedSource.begin(members, extraction, compare));
      DictionaryMembersValidation semantic(stagedSource, scratch, &decoder, window);
      DictionaryMembersDetails details;
      ASSERT_TRUE(semantic.validate(members.compressed, members.hasSynonyms, details));
      EXPECT_EQ(details.info.words, 2u);
      EXPECT_EQ(details.compressed, members.compressed);
      HalDictionaryStagedArchive canonical(scratch, &decoder, window);
      ContentManifest canonicalManifest;
      ASSERT_TRUE(canonical.build(members, extraction, canonicalManifest));
      ASSERT_NE(canonical.current(), nullptr);
      EXPECT_EQ(canonicalManifest.kind, ContentKind::Dictionary);
      EXPECT_EQ(canonicalManifest.formatVersion, 1u);
      EXPECT_EQ(canonicalManifest.logicalIdentity, Identity{});
      const auto canonicalBytes = inventory_hal_test::state.files.at(DICTIONARY_CACHE_CANDIDATE);
      EXPECT_EQ(canonicalBytes.size(), canonicalManifest.length);
      HalFile canonicalFile;
      ASSERT_TRUE(Storage.openFileForReadReusing("TEST", DICTIONARY_CACHE_CANDIDATE, canonicalFile));
      HalInventoryFileView canonicalSource;
      ASSERT_TRUE(canonicalSource.attach(canonicalFile));
      ZipNormalizedNamesValidation canonicalNames(canonicalSource, duplicates, workspace);
      ZipArchiveValidation canonicalValidation(canonicalSource, scratch, &decoder, window);
      ZipDirectoryLayout canonicalLayout;
      ASSERT_TRUE(canonicalValidation.validate(canonicalLayout, &ranges, &canonicalNames));
      EXPECT_EQ(canonicalLayout.entries, 4u);
      uint64_t canonicalLength = 0;
      Digest canonicalHash{};
      ASSERT_TRUE(hashInventoryFile(canonicalFile, compare, canonicalLength, canonicalHash));
      EXPECT_EQ(canonicalHash, canonicalManifest.contentHash);
      const auto unchangedManifest = canonicalManifest;
      EXPECT_FALSE(canonical.build(members, extraction, canonicalManifest));
      EXPECT_EQ(canonicalManifest, unchangedManifest);
      EXPECT_EQ(canonical.current(), nullptr);
      EXPECT_EQ(inventory_hal_test::state.files.at(DICTIONARY_CACHE_CANDIDATE), canonicalBytes);
      ASSERT_TRUE(canonicalFile.close());
      HalDictionaryCacheStorage cache;
      DictionaryCachePublication canonicalPublication(cache, compare);
      ASSERT_EQ(canonicalPublication.publish(unchangedManifest), DictionaryCacheResult::Ok);
      EXPECT_EQ(inventory_hal_test::state.files.at(canonicalPublication.publishedPath()), canonicalBytes);
      HalDictionaryOriginalArchive original(cache, scratch);
      HalDictionaryOriginalArchive cancelledOriginal(cache, scratch, [](void*) { return false; });
      EXPECT_FALSE(cancelledOriginal.retain(source, manifest, recoveredParent));
      EXPECT_EQ(cancelledOriginal.publishedPath(), nullptr);
      EXPECT_FALSE(inventory_hal_test::state.files.contains(DICTIONARY_CACHE_CANDIDATE));
      inventory_hal_test::state.failSync = true;
      EXPECT_FALSE(original.retain(source, manifest, recoveredParent));
      EXPECT_EQ(original.publishedPath(), nullptr);
      EXPECT_FALSE(inventory_hal_test::state.files.contains(DICTIONARY_CACHE_CANDIDATE));
      inventory_hal_test::state.failSync = false;
      ASSERT_TRUE(original.retain(source, manifest, recoveredParent));
      ASSERT_NE(original.publishedPath(), nullptr);
      EXPECT_EQ(inventory_hal_test::state.files.at(original.publishedPath()), bytes);
      EXPECT_NE(manifest.contentHash, unchangedManifest.contentHash);
      EXPECT_FALSE(inventory_hal_test::state.files.contains(DICTIONARY_CACHE_CANDIDATE));
      inventory_hal_test::state.files[DICTIONARY_CACHE_CANDIDATE] = {9};
      EXPECT_TRUE(original.retain(source, manifest, recoveredParent));
      EXPECT_EQ(inventory_hal_test::state.files.at(DICTIONARY_CACHE_CANDIDATE), std::vector<uint8_t>{9});
      inventory_hal_test::state.files.erase(DICTIONARY_CACHE_CANDIDATE);
      ASSERT_TRUE(canonical.build(members, extraction, canonicalManifest));
      ASSERT_EQ(canonicalPublication.publish(canonicalManifest), DictionaryCacheResult::Ok);
      ASSERT_TRUE(canonical.matchesReceipt(*recoveredParent.current()));
      auto unrelated = *recoveredParent.current();
      unrelated.hashes[0][0] ^= 1;
      EXPECT_FALSE(canonical.matchesReceipt(unrelated));
      HalDictionaryBindings bindings(cache, receiptScratch);
      HalDictionaryBindingInstallation bindingInstallation(bindings);
      static constexpr const char* BASE = "/dictionaries/imported/dictionary";
      HalDictionaryInstallationPlanBuilder planBuilder;
      DictionaryInstallationPlan installationPlan;
      installationPlan.revision = 99;
      const auto unchangedPlan = installationPlan;
      HalDictionaryStagedArchive unbuiltCanonical(scratch, &decoder, window);
      EXPECT_FALSE(planBuilder.create(BASE, manifest, unbuiltCanonical, recoveredParent, installationPlan));
      EXPECT_EQ(installationPlan, unchangedPlan);
      EXPECT_FALSE(planBuilder.create("/books/dictionary", manifest, canonical, recoveredParent, installationPlan));
      EXPECT_EQ(installationPlan, unchangedPlan);
      auto mismatchedManifest = manifest;
      mismatchedManifest.contentHash[0] ^= 1;
      EXPECT_FALSE(planBuilder.create(BASE, mismatchedManifest, canonical, recoveredParent, installationPlan));
      EXPECT_EQ(installationPlan, unchangedPlan);
      ASSERT_TRUE(planBuilder.create(BASE, manifest, canonical, recoveredParent, installationPlan));
      EXPECT_EQ(installationPlan.revision, 1u);
      EXPECT_EQ(installationPlan.phase, DictionaryInstallationPhase::Prepared);
      EXPECT_EQ(installationPlan.published, 0u);
      EXPECT_EQ(installationPlan.extraction, *recoveredParent.current());
      EXPECT_EQ(installationPlan.archives.members, canonicalManifest);
      EXPECT_EQ(installationPlan.archives.original, manifest);
      ASSERT_TRUE(planBuilder.create(std::string_view(installationPlan.base.data()), installationPlan.archives.original,
                                     canonical, recoveredParent, installationPlan));
      EXPECT_STREQ(installationPlan.base.data(), BASE);
      HalDictionaryExtractionJournalStorage planStorage(HalDictionaryExtractionJournalStorage::Purpose::Installation);
      std::array<uint8_t, DICTIONARY_INSTALLATION_PLAN_SIZE> planScratch{};
      DictionaryInstallationJournal installJournal(planStorage, planScratch);
      ASSERT_EQ(installJournal.begin(installationPlan), DictionaryJournalResult::Ok);
      DictionaryInstallationJournal recoveredInstall(planStorage, planScratch);
      ASSERT_EQ(recoveredInstall.recover(installationPlan), DictionaryJournalResult::Ok);
      EXPECT_EQ(*recoveredInstall.current(), installationPlan);
      const auto beforeBinding = inventory_hal_test::state.files;
      EXPECT_FALSE(bindingInstallation.install(BASE, manifest, canonical, recoveredParent));
      EXPECT_EQ(inventory_hal_test::state.files, beforeBinding);
      parentState.phase = TransferPhase::Installing;
      auto wrongOriginal = manifest;
      wrongOriginal.contentHash[0] ^= 1;
      EXPECT_FALSE(bindingInstallation.install(BASE, wrongOriginal, canonical, recoveredParent));
      EXPECT_EQ(inventory_hal_test::state.files, beforeBinding);
      ASSERT_TRUE(bindingInstallation.install(BASE, manifest, canonical, recoveredParent));
      EXPECT_FALSE(bindingInstallation.finalize(BASE, manifest, canonical, recoveredParent));
      parentState.phase = TransferPhase::Committed;
      ASSERT_TRUE(bindingInstallation.finalize(BASE, manifest, canonical, recoveredParent));
      DictionaryArchiveBinding bound;
      ASSERT_EQ(bindings.read(BASE, bound), DictionaryBindingResult::Found);
      EXPECT_EQ(bound.members, canonicalManifest);
      EXPECT_EQ(bound.original, manifest);
      const auto indexPath = HalZipEntryStage::memberPath(HalZipEntryStage::Member::Index);
      inventory_hal_test::state.files.at(indexPath)[0] ^= 1;
      EXPECT_FALSE(stagedSource.begin(members, extraction, compare));
      inventory_hal_test::state.files.at(indexPath)[0] ^= 1;
      ASSERT_TRUE(stagedSource.begin(members, extraction, compare));
      ASSERT_TRUE(stagedSource.close());
      auto& indexBytes = inventory_hal_test::state.files.at(indexPath);
      indexBytes.push_back(0);
      EXPECT_FALSE(stagedSource.begin(members, extraction, compare));
      indexBytes.pop_back();
      inventory_hal_test::state.readErrorPath = indexPath;
      EXPECT_FALSE(stagedSource.begin(members, extraction, compare));
      inventory_hal_test::state.readErrorPath.clear();
      EXPECT_FALSE(stagedSource.begin(members, extraction, compare, [](void*) { return false; }));
      ASSERT_TRUE(stagedSource.begin(members, extraction, compare));
      inventory_hal_test::state.failClosePath = indexPath;
      EXPECT_FALSE(stagedSource.close());
      uint64_t unavailable = 99;
      EXPECT_FALSE(stagedSource.size(1, unavailable));
      EXPECT_EQ(unavailable, 99u);
      inventory_hal_test::state.failClosePath.clear();
      ASSERT_TRUE(stagedSource.close());
      const std::array<ZipEntrySpan, 4> payloads{members.definitions, members.index, members.info, members.synonyms};
      const std::array<HalZipEntryStage::Member, 4> roles{
          HalZipEntryStage::Member::Definitions, HalZipEntryStage::Member::Index, HalZipEntryStage::Member::Info,
          HalZipEntryStage::Member::Synonyms};
      for (size_t at = 0; at < roles.size(); ++at) {
        const auto& payload = payloads[at];
        ASSERT_EQ(payload.method, 0u);
        EXPECT_NE(extraction.contentHash(roles[at]), nullptr);
        const auto& staged = inventory_hal_test::state.files.at(HalZipEntryStage::memberPath(roles[at]));
        ASSERT_EQ(staged.size(), payload.expandedBytes);
        EXPECT_TRUE(std::equal(staged.begin(), staged.end(), bytes.begin() + payload.offset));
      }
      // Earlier sealed members are retained when a later extraction fails. The
      // parent installer owns their cleanup; no installed dictionary is changed.
      const auto infoPath = HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info);
      inventory_hal_test::state.files.erase(infoPath);
      inventory_hal_test::state.files.at("/source.zip")[members.info.offset] ^= 1;
      EXPECT_FALSE(stage.extractMember(extractor, members.info, HalZipEntryStage::Member::Info));
      EXPECT_FALSE(stage.isSealed());
      EXPECT_FALSE(inventory_hal_test::state.files.contains(infoPath));
      EXPECT_TRUE(inventory_hal_test::state.files.contains(HalZipEntryStage::memberPath(roles[0])));
      EXPECT_TRUE(inventory_hal_test::state.files.contains(HalZipEntryStage::memberPath(roles[1])));
      inventory_hal_test::state.files.at("/source.zip") = bytes;
      ASSERT_TRUE(stage.extractMember(extractor, members.info, HalZipEntryStage::Member::Info));
      // A completed selector rejects subsequent visits. The enclosing archive must
      // propagate that rejection without returning a successful layout.
      layout.entries = 99;
      EXPECT_FALSE(archive.validate(layout, &ranges, &names));
      EXPECT_EQ(layout.entries, 99u);
      EXPECT_FALSE(names.finish());
      EXPECT_FALSE(ranges.finish());
      selection.begin();
      EXPECT_TRUE(archive.validate(layout, &ranges, &names));
      EXPECT_EQ(inventory_hal_test::state.files.at("/source.zip"), bytes);
    }
  }
}

TEST_F(HalZipNormalizedTest, ReceiptProviderPreservesDirectoriesAndFailsClosedOnStorageFaults) {
  std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> receiptScratch{};
  DictionaryExtractionReceipt initial;
  initial.revision = 1;
  initial.transaction[0] = 1;
  initial.generation[0] = 2;
  initial.archiveHash[0] = 3;
  initial.lengths = {6, 24, 111, 0};
  for (unsigned fault = 0; fault < 5; ++fault) {
    inventory_hal_test::state = {};
    HalDictionaryExtractionJournalStorage storage;
    DictionaryExtractionJournal journal(storage, receiptScratch);
    auto& state = inventory_hal_test::state;
    if (fault == 0) state.directories[DICTIONARY_EXTRACTION_JOURNALS[0]] = {};
    if (fault == 1) state.failSync = true;
    if (fault == 2) state.failTruncate = true;
    if (fault == 3) state.corruptWrite = true;
    if (fault == 4) state.failClosePath = DICTIONARY_EXTRACTION_JOURNALS[0];
    EXPECT_NE(journal.begin(initial), DictionaryJournalResult::Ok) << fault;
    EXPECT_EQ(journal.current(), nullptr);
    if (fault == 0) {
      EXPECT_TRUE(state.directories.contains(DICTIONARY_EXTRACTION_JOURNALS[0]));
    }
    state.failSync = state.failTruncate = state.corruptWrite = false;
    state.failClosePath.clear();
  }
}
TEST_F(HalZipNormalizedTest, InstallationJournalRetainsHandlesAndCoexistsWithExtractionSlots) {
  const auto plan = initialPlan();
  std::array<uint8_t, DICTIONARY_INSTALLATION_PLAN_SIZE> planScratch{};
  HalDictionaryExtractionJournalStorage plans(HalDictionaryExtractionJournalStorage::Purpose::Installation);
  DictionaryInstallationJournal journal(plans, planScratch);
  ASSERT_EQ(journal.begin(plan), DictionaryJournalResult::Ok);
  ASSERT_EQ(journal.startPublishing(), DictionaryJournalResult::Ok);
  for (unsigned at : {2u, 1u, 0u}) ASSERT_EQ(journal.recordPublished(at), DictionaryJournalResult::Ok);
  ASSERT_EQ(journal.markBound(), DictionaryJournalResult::Ok);
  ASSERT_EQ(journal.markCommitted(), DictionaryJournalResult::Ok);
  DictionaryInstallationJournal restarted(plans, planScratch);
  ASSERT_EQ(restarted.recover(plan), DictionaryJournalResult::Ok);
  EXPECT_EQ(*restarted.current(), *journal.current());
  EXPECT_EQ(inventory_hal_test::state.preparations, 3u);
  HalDictionaryExtractionJournalStorage extraction;
  std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> receiptScratch{};
  ASSERT_EQ(encodeDictionaryExtractionReceipt(plan.extraction, receiptScratch), receiptScratch.size());
  ASSERT_TRUE(extraction.write(DICTIONARY_EXTRACTION_JOURNALS[0], 0, receiptScratch, true));
  const auto unchanged = inventory_hal_test::state.files;
  EXPECT_FALSE(plans.write(DICTIONARY_EXTRACTION_JOURNALS[0], 0, planScratch, true));
  EXPECT_FALSE(extraction.write(DICTIONARY_INSTALLATION_JOURNALS[0], 0, receiptScratch, true));
  EXPECT_FALSE(plans.write(DICTIONARY_INSTALLATION_JOURNALS[0], 0, receiptScratch, true));
  EXPECT_EQ(inventory_hal_test::state.files, unchanged);
  EXPECT_EQ(inventory_hal_test::state.preparations, 6u);
}
TEST_F(HalZipNormalizedTest, InstallationJournalStorageFaultsAndUnknownPathsFailClosed) {
  const auto plan = initialPlan();
  std::array<uint8_t, DICTIONARY_INSTALLATION_PLAN_SIZE> planScratch{};
  for (unsigned fault = 0; fault < 6; ++fault) {
    inventory_hal_test::state = {};
    HalDictionaryExtractionJournalStorage storage(HalDictionaryExtractionJournalStorage::Purpose::Installation);
    DictionaryInstallationJournal journal(storage, planScratch);
    auto& state = inventory_hal_test::state;
    if (fault == 0) state.directories[DICTIONARY_INSTALLATION_JOURNALS[0]] = {};
    if (fault == 1) state.failSync = true;
    if (fault == 2) state.failTruncate = true;
    if (fault == 3) state.corruptWrite = true;
    if (fault == 4) state.failClosePath = DICTIONARY_INSTALLATION_JOURNALS[0];
    if (fault == 5) state.files[DICTIONARY_INSTALLATION_JOURNALS[0]] = {1, 2, 3};
    EXPECT_NE(journal.begin(plan), DictionaryJournalResult::Ok) << fault;
    EXPECT_EQ(journal.current(), nullptr);
    if (fault == 0) {
      EXPECT_TRUE(state.directories.contains(DICTIONARY_INSTALLATION_JOURNALS[0]));
    }
    if (fault == 5) {
      EXPECT_EQ(state.files.at(DICTIONARY_INSTALLATION_JOURNALS[0]), (std::vector<uint8_t>{1, 2, 3}));
    }
    state.failSync = state.failTruncate = state.corruptWrite = false;
    state.failClosePath.clear();
  }
}

}  // namespace

TEST_F(HalZipNormalizedTest, ProductionDictionarySelectionValidatesBothPassesAndCleansPrivateIndexes) {
  for (const char* fixture : {"DictionaryBundle-plain.fixture", "DictionaryBundle-dictzip.fixture"}) {
    SetUp();
    std::ifstream input(std::string(ZIP_NORMALIZED_DIR) + "/" + fixture, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    ASSERT_FALSE(bytes.empty());
    inventory_hal_test::state.files["/source.zip"] = bytes;
    HalFile file;
    ASSERT_TRUE(Storage.openFileForReadReusing("TEST", "/source.zip", file));
    HalInventoryFileView source;
    ASSERT_TRUE(source.attach(file));
    auto selector =
        makeUniqueNoThrow<HalDictionaryArchiveSelection>(source, scratch, compare, &decoder, window, workspace);
    ASSERT_TRUE(selector);
    DictionaryZipMembers result;
    ASSERT_TRUE(selector->select(result));
    EXPECT_EQ(result.compressed, std::string(fixture) == "DictionaryBundle-dictzip.fixture");
    EXPECT_TRUE(result.hasSynonyms);
    EXPECT_GT(result.info.expandedBytes, 0u);
    for (const char* path :
         {HalZipRangeStorage::PATH, HalZipRangeStorage::NAME_INDEX_PATH, HalZipNameBytesStorage::PATH})
      EXPECT_FALSE(inventory_hal_test::state.files.contains(path));
    EXPECT_EQ(inventory_hal_test::state.files.at("/source.zip"), bytes);
    DictionaryZipMembers repeated;
    ASSERT_TRUE(selector->select(repeated));
    EXPECT_EQ(repeated.info.offset, result.info.offset);
    EXPECT_EQ(repeated.definitions.offset, result.definitions.offset);
  }
}

TEST_F(HalZipNormalizedTest, ProductionDictionarySelectionRejectsDuplicatesAndCancellationWithoutPublishing) {
  for (const char* fixture : {"ZipNames-duplicate.fixture", "DictionaryBundle-plain.fixture"}) {
    SetUp();
    std::ifstream input(std::string(ZIP_NORMALIZED_DIR) + "/" + fixture, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    inventory_hal_test::state.files["/source.zip"] = bytes;
    HalFile file;
    ASSERT_TRUE(Storage.openFileForReadReusing("TEST", "/source.zip", file));
    HalInventoryFileView source;
    ASSERT_TRUE(source.attach(file));
    bool cancel = std::string(fixture) == "DictionaryBundle-plain.fixture";
    auto selector = makeUniqueNoThrow<HalDictionaryArchiveSelection>(
        source, scratch, compare, &decoder, window, workspace,
        [](void* context) { return !*static_cast<bool*>(context); }, &cancel);
    ASSERT_TRUE(selector);
    DictionaryZipMembers output;
    output.info.offset = 123456;
    EXPECT_FALSE(selector->select(output));
    EXPECT_EQ(output.info.offset, 123456u);
    EXPECT_EQ(inventory_hal_test::state.files.at("/source.zip"), bytes);
  }
}

TEST_F(HalZipNormalizedTest, ProductionDictionarySelectionCancelsAtEveryProgressBoundary) {
  std::ifstream input(std::string(ZIP_NORMALIZED_DIR) + "/DictionaryBundle-plain.fixture", std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  ASSERT_FALSE(bytes.empty());
  struct Control {
    unsigned calls = 0, stop = 0;
  } control;
  unsigned boundaries = 0;
  for (unsigned stop = 0; stop <= boundaries; ++stop) {
    SetUp();
    control = {0, stop};
    inventory_hal_test::state.files["/source.zip"] = bytes;
    HalFile file;
    ASSERT_TRUE(Storage.openFileForReadReusing("TEST", "/source.zip", file));
    HalInventoryFileView source;
    ASSERT_TRUE(source.attach(file));
    auto selector = makeUniqueNoThrow<HalDictionaryArchiveSelection>(
        source, scratch, compare, &decoder, window, workspace,
        [](void* context) {
          auto& control = *static_cast<Control*>(context);
          return ++control.calls != control.stop;
        },
        &control);
    ASSERT_TRUE(selector);
    DictionaryZipMembers output;
    output.info.offset = 123456;
    if (stop == 0) {
      ASSERT_TRUE(selector->select(output));
      boundaries = control.calls;
      ASSERT_GT(boundaries, 0u);
    } else {
      EXPECT_FALSE(selector->select(output)) << stop;
      EXPECT_EQ(output.info.offset, 123456u) << stop;
    }
    EXPECT_EQ(inventory_hal_test::state.files.at("/source.zip"), bytes);
    selector.reset();
    for (const char* path :
         {HalZipRangeStorage::PATH, HalZipRangeStorage::NAME_INDEX_PATH, HalZipNameBytesStorage::PATH})
      EXPECT_FALSE(inventory_hal_test::state.files.contains(path)) << stop;
  }
}

TEST_F(HalZipNormalizedTest, IncomingDictionaryExtractionPersistsOwnershipAndReopensCompletedMembers) {
  for (const char* fixture : {"DictionaryBundle-plain.fixture", "DictionaryBundle-dictzip.fixture"}) {
    SetUp();
    std::ifstream input(std::string(ZIP_NORMALIZED_DIR) + "/" + fixture, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    inventory_hal_test::state.files["/source.zip"] = bytes;
    HalFile file;
    ASSERT_TRUE(Storage.openFileForReadReusing("TEST", "/source.zip", file));
    ContentManifest manifest;
    manifest.kind = ContentKind::Dictionary;
    manifest.formatVersion = 1;
    ASSERT_TRUE(hashInventoryFile(file, compare, manifest.length, manifest.contentHash));
    TransferState state;
    state.owner[0] = 1;
    state.transaction[0] = 2;
    state.storageGeneration[0] = 3;
    state.length = state.durableOffset = manifest.length;
    state.contentHash = manifest.contentHash;
    std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> receiptScratch{};
    auto incoming = makeUniqueNoThrow<HalDictionaryIncomingExtraction>(
        file, state, manifest, state.storageGeneration, "/dictionaries/imported/dictionary", scratch, compare,
        receiptScratch, &decoder, window, workspace);
    ASSERT_TRUE(incoming);
    ASSERT_TRUE(incoming->prepare());
    ASSERT_NE(incoming->selectedMembers(), nullptr);
    ASSERT_NE(incoming->extractedMembers(), nullptr);
    ASSERT_NE(incoming->receiptParent(), nullptr);
    EXPECT_EQ(incoming->receiptParent()->current()->sealed, 15);
    EXPECT_EQ(incoming->selectedMembers()->compressed, std::string(fixture) == "DictionaryBundle-dictzip.fixture");
    const auto selected = *incoming->selectedMembers();
    incoming.reset();
    const auto saved = inventory_hal_test::state.files;
    auto reopened = makeUniqueNoThrow<HalDictionaryIncomingExtraction>(
        file, state, manifest, state.storageGeneration, "/dictionaries/imported/dictionary", scratch, compare,
        receiptScratch, &decoder, window, workspace);
    ASSERT_TRUE(reopened);
    ASSERT_TRUE(reopened->prepare());
    EXPECT_EQ(inventory_hal_test::state.files, saved);
    EXPECT_EQ(reopened->selectedMembers()->definitions.offset, selected.definitions.offset);
    EXPECT_EQ(reopened->receiptParent()->current()->sealed, 15);
    auto preparation =
        makeUniqueNoThrow<HalDictionaryInstallationPreparation>(*reopened, file, manifest, compare, &decoder, window);
    ASSERT_TRUE(preparation);
    ASSERT_TRUE(preparation->prepare("/dictionaries/imported/dictionary"));
    ASSERT_NE(preparation->current(), nullptr);
    HalDictionaryCacheStorage cache;
    DictionaryCachePublication publication(cache, compare);
    EXPECT_EQ(preparation->current()->archives.original, manifest);
    ASSERT_EQ(publication.find(preparation->current()->archives.members), DictionaryCacheResult::Ok);
    ASSERT_EQ(publication.find(manifest), DictionaryCacheResult::Ok);
    state.phase = TransferPhase::Installing;
    HalDictionaryExtractionJournalStorage durableReceipts;
    DictionaryExtractionJournal recoveredJournal(durableReceipts, receiptScratch);
    DictionaryExtractionParent recoveredParent(recoveredJournal, state, manifest, state.storageGeneration);
    const auto durableFiles = inventory_hal_test::state.files;
    ASSERT_EQ(recoveredParent.recover(), DictionaryJournalResult::Ok);
    ASSERT_NE(recoveredParent.current(), nullptr);
    EXPECT_EQ(recoveredParent.current()->sealed, 15u);
    state.phase = TransferPhase::Committed;
    ASSERT_EQ(recoveredParent.recover(), DictionaryJournalResult::Ok);
    EXPECT_EQ(inventory_hal_test::state.files, durableFiles);
    state.phase = TransferPhase::Installing;
    EXPECT_FALSE(reopened->prepare());
    EXPECT_EQ(reopened->selectedMembers(), nullptr);
    EXPECT_EQ(preparation->current(), nullptr);
  }
}

TEST_F(HalZipNormalizedTest, IncomingDictionaryExtractionRejectsWrongSourceAndSealedMemberCorruption) {
  std::ifstream input(std::string(ZIP_NORMALIZED_DIR) + "/DictionaryBundle-plain.fixture", std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  inventory_hal_test::state.files["/source.zip"] = bytes;
  HalFile file;
  ASSERT_TRUE(Storage.openFileForReadReusing("TEST", "/source.zip", file));
  ContentManifest manifest;
  manifest.kind = ContentKind::Dictionary;
  manifest.formatVersion = 1;
  ASSERT_TRUE(hashInventoryFile(file, compare, manifest.length, manifest.contentHash));
  TransferState state;
  state.owner[0] = 1;
  state.transaction[0] = 2;
  state.storageGeneration[0] = 3;
  state.length = state.durableOffset = manifest.length;
  state.contentHash = manifest.contentHash;
  std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> receiptScratch{};
  auto incoming = makeUniqueNoThrow<HalDictionaryIncomingExtraction>(
      file, state, manifest, state.storageGeneration, "/dictionaries/imported/dictionary", scratch, compare,
      receiptScratch, &decoder, window, workspace);
  ASSERT_TRUE(incoming);
  inventory_hal_test::state.files.at("/source.zip")[0] ^= 1;
  const auto wrongSource = inventory_hal_test::state.files;
  EXPECT_FALSE(incoming->prepare());
  EXPECT_EQ(inventory_hal_test::state.files, wrongSource);
  EXPECT_EQ(incoming->receiptParent(), nullptr);
  inventory_hal_test::state.files.at("/source.zip")[0] ^= 1;
  ASSERT_TRUE(incoming->prepare());
  const auto path = HalZipEntryStage::memberPath(HalZipEntryStage::Member::Index);
  inventory_hal_test::state.files.at(path)[0] ^= 1;
  const auto brokenMember = inventory_hal_test::state.files;
  EXPECT_FALSE(incoming->prepare());
  EXPECT_EQ(inventory_hal_test::state.files, brokenMember);
  EXPECT_EQ(incoming->extractedMembers(), nullptr);
  inventory_hal_test::state.files.at(path)[0] ^= 1;
  ASSERT_TRUE(incoming->prepare());
}

TEST_F(HalZipNormalizedTest, IncomingDictionaryExtractionRecoversFailedReceiptSyncAndPartialMember) {
  std::ifstream input(std::string(ZIP_NORMALIZED_DIR) + "/DictionaryBundle-plain.fixture", std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  for (const char* failedReceipt : DICTIONARY_EXTRACTION_JOURNALS) {
    SetUp();
    inventory_hal_test::state.files["/source.zip"] = bytes;
    HalFile file;
    ASSERT_TRUE(Storage.openFileForReadReusing("TEST", "/source.zip", file));
    ContentManifest manifest;
    manifest.kind = ContentKind::Dictionary;
    manifest.formatVersion = 1;
    ASSERT_TRUE(hashInventoryFile(file, compare, manifest.length, manifest.contentHash));
    TransferState state;
    state.owner[0] = 1;
    state.transaction[0] = 2;
    state.storageGeneration[0] = 3;
    state.length = state.durableOffset = manifest.length;
    state.contentHash = manifest.contentHash;
    std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> receiptScratch{};
    auto incoming = makeUniqueNoThrow<HalDictionaryIncomingExtraction>(
        file, state, manifest, state.storageGeneration, "/dictionaries/imported/dictionary", scratch, compare,
        receiptScratch, &decoder, window, workspace);
    ASSERT_TRUE(incoming);
    inventory_hal_test::state.failSyncPath = failedReceipt;
    EXPECT_FALSE(incoming->prepare());
    EXPECT_EQ(incoming->receiptParent(), nullptr);
    const auto infoPath = HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info);
    const bool initial = std::string(failedReceipt) == DICTIONARY_EXTRACTION_JOURNALS[0];
    EXPECT_EQ(inventory_hal_test::state.files.contains(infoPath), !initial);
    std::vector<uint8_t> retainedInfo;
    if (!initial) retainedInfo = inventory_hal_test::state.files.at(infoPath);
    incoming.reset();
    inventory_hal_test::state.failSyncPath.clear();
    if (initial) inventory_hal_test::state.files[infoPath] = {1, 2, 3};
    auto reopened = makeUniqueNoThrow<HalDictionaryIncomingExtraction>(
        file, state, manifest, state.storageGeneration, "/dictionaries/imported/dictionary", scratch, compare,
        receiptScratch, &decoder, window, workspace);
    ASSERT_TRUE(reopened);
    ASSERT_TRUE(reopened->prepare());
    EXPECT_EQ(reopened->receiptParent()->current()->sealed, 15);
    if (!initial) {
      EXPECT_EQ(inventory_hal_test::state.files.at(infoPath), retainedInfo);
    }
    EXPECT_EQ(inventory_hal_test::state.files.at("/source.zip"), bytes);
    for (unsigned member = 0; member < 4; ++member) {
      const auto path = HalZipEntryStage::memberPath(static_cast<HalZipEntryStage::Member>(member));
      EXPECT_EQ(inventory_hal_test::state.files.at(path).size(), reopened->receiptParent()->current()->lengths[member]);
    }
  }
}

TEST_F(HalZipNormalizedTest, InstallationPreparationPreservesMembersAndRetriesWithFreshOwner) {
  std::ifstream input(std::string(ZIP_NORMALIZED_DIR) + "/DictionaryBundle-plain.fixture", std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  for (unsigned fault = 0; fault < 3; ++fault) {
    SetUp();
    inventory_hal_test::state.files["/source.zip"] = bytes;
    HalFile file;
    ASSERT_TRUE(Storage.openFileForReadReusing("TEST", "/source.zip", file));
    ContentManifest manifest;
    manifest.kind = ContentKind::Dictionary;
    manifest.formatVersion = 1;
    ASSERT_TRUE(hashInventoryFile(file, compare, manifest.length, manifest.contentHash));
    TransferState state;
    state.owner[0] = 1;
    state.transaction[0] = 2;
    state.storageGeneration[0] = 3;
    state.length = state.durableOffset = manifest.length;
    state.contentHash = manifest.contentHash;
    std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> receiptScratch{};
    auto incoming = makeUniqueNoThrow<HalDictionaryIncomingExtraction>(
        file, state, manifest, state.storageGeneration, "/dictionaries/imported/dictionary", scratch, compare,
        receiptScratch, &decoder, window, workspace);
    ASSERT_TRUE(incoming);
    ASSERT_TRUE(incoming->prepare());
    const auto info = HalZipEntryStage::memberPath(HalZipEntryStage::Member::Info);
    if (fault == 1) inventory_hal_test::state.failSyncPath = DICTIONARY_CACHE_CANDIDATE;
    if (fault == 2) inventory_hal_test::state.files.at(info)[0] ^= 1;
    const auto before = inventory_hal_test::state.files;
    auto preparation =
        makeUniqueNoThrow<HalDictionaryInstallationPreparation>(*incoming, file, manifest, compare, &decoder, window);
    ASSERT_TRUE(preparation);
    EXPECT_FALSE(preparation->prepare(fault == 0 ? "/books/dictionary" : "/dictionaries/imported/dictionary"));
    EXPECT_EQ(preparation->current(), nullptr);
    EXPECT_EQ(inventory_hal_test::state.files, before);
    preparation.reset();
    inventory_hal_test::state.failSyncPath.clear();
    if (fault == 2) inventory_hal_test::state.files.at(info)[0] ^= 1;
    // Disposable staging may remain after a power cut; retained blobs are distinct.
    inventory_hal_test::state.files[DICTIONARY_CACHE_CANDIDATE] = {9, 8, 7};
    auto retry =
        makeUniqueNoThrow<HalDictionaryInstallationPreparation>(*incoming, file, manifest, compare, &decoder, window);
    ASSERT_TRUE(retry);
    ASSERT_TRUE(retry->prepare("/dictionaries/imported/dictionary"));
    ASSERT_NE(retry->current(), nullptr);
    EXPECT_EQ(retry->current()->archives.original, manifest);
    EXPECT_FALSE(inventory_hal_test::state.files.contains(DICTIONARY_CACHE_CANDIDATE));
    EXPECT_EQ(inventory_hal_test::state.files.at("/source.zip"), bytes);
    const auto prepared = inventory_hal_test::state.files;
    EXPECT_FALSE(retry->prepare("/dictionaries/other/dictionary"));
    EXPECT_EQ(retry->current(), nullptr);
    EXPECT_EQ(inventory_hal_test::state.files, prepared);
  }
}

TEST_F(HalZipNormalizedTest, ZipAuditOwnsAllIndexesAcrossRestartAndInterruptedCleanup) {
  for (unsigned fault = 0; fault < 4; ++fault) {
    SetUp();
    inventory_hal_test::state.enumerateFileMap = true;
    inventory_hal_test::state.directories["/"] = {};
    ContentManifest manifest;
    manifest.kind = ContentKind::Dictionary;
    manifest.formatVersion = 1;
    manifest.length = 100;
    manifest.contentHash.fill(7);
    TransferState state;
    state.transaction.fill(1);
    state.owner.fill(2);
    state.storageGeneration.fill(3);
    state.contentHash = manifest.contentHash;
    state.length = state.durableOffset = manifest.length;
    constexpr const char* base = "/dictionaries/imported/dictionary";
    auto audit = makeUniqueNoThrow<HalDictionaryZipAudit>(state, manifest, state.storageGeneration, base, compare);
    ASSERT_TRUE(audit);
    ASSERT_TRUE(audit->begin());
    const auto initialized = inventory_hal_test::state.files;
    EXPECT_FALSE(audit->abort());
    EXPECT_EQ(inventory_hal_test::state.files, initialized);
    audit.reset();
    for (const auto path :
         {HalZipRangeStorage::PATH, HalZipRangeStorage::NAME_INDEX_PATH, HalZipNameBytesStorage::PATH})
      inventory_hal_test::state.files[path] = {1, 2, 3};
    if (fault == 1) inventory_hal_test::state.failRemoveAfter = true;
    if (fault == 2) state.phase = TransferPhase::Aborted;
    if (fault == 3) {
      inventory_hal_test::state.directories[HalZipNameBytesStorage::PATH] = {};
      inventory_hal_test::state.files.erase(HalZipNameBytesStorage::PATH);
    }
    const auto saved = inventory_hal_test::state.files;
    auto fresh = makeUniqueNoThrow<HalDictionaryZipAudit>(state, manifest, state.storageGeneration, base, compare);
    ASSERT_TRUE(fresh);
    if (fault == 2) {
      EXPECT_FALSE(fresh->begin());
      EXPECT_EQ(inventory_hal_test::state.files, saved);
      EXPECT_TRUE(fresh->abort());
    } else if (fault == 1 || fault == 3) {
      EXPECT_FALSE(fresh->begin());
      EXPECT_TRUE(inventory_hal_test::state.files.contains(DICTIONARY_ZIP_AUDIT_JOURNALS[0]));
      if (fault == 3) {
        EXPECT_EQ(inventory_hal_test::state.files, saved);
        inventory_hal_test::state.directories.erase(HalZipNameBytesStorage::PATH);
      }
      fresh.reset();
      inventory_hal_test::state.failRemoveAfter = false;
      fresh = makeUniqueNoThrow<HalDictionaryZipAudit>(state, manifest, state.storageGeneration, base, compare);
      ASSERT_TRUE(fresh && fresh->begin() && fresh->finish());
    } else {
      ASSERT_TRUE(fresh->begin() && fresh->finish());
    }
    for (const auto path : DICTIONARY_ZIP_AUDIT_JOURNALS) EXPECT_FALSE(inventory_hal_test::state.files.contains(path));
    for (const auto path :
         {HalZipRangeStorage::PATH, HalZipRangeStorage::NAME_INDEX_PATH, HalZipNameBytesStorage::PATH})
      EXPECT_FALSE(inventory_hal_test::state.files.contains(path));
  }
}

TEST_F(HalZipNormalizedTest, ZipAuditNeverRemovesFilesAfterParentChangesInProgressCallback) {
  struct Mutation {
    TransferState* parent;
    unsigned remaining;
    bool armed = false, changed = false;
    std::map<std::string, std::vector<uint8_t>> saved;
  };
  const auto progress = [](void* context) {
    auto& mutation = *static_cast<Mutation*>(context);
    if (mutation.armed && --mutation.remaining == 0) {
      mutation.saved = inventory_hal_test::state.files;
      mutation.parent->owner[0] ^= 1;
      mutation.armed = false;
      mutation.changed = true;
    }
    return true;
  };
  for (unsigned boundary = 1; boundary <= 64; ++boundary) {
    SetUp();
    inventory_hal_test::state.enumerateFileMap = true;
    inventory_hal_test::state.directories["/"] = {};
    ContentManifest manifest;
    manifest.kind = ContentKind::Dictionary;
    manifest.formatVersion = 1;
    manifest.length = 100;
    manifest.contentHash.fill(7);
    TransferState state;
    state.transaction.fill(1);
    state.owner.fill(2);
    state.storageGeneration.fill(3);
    state.contentHash = manifest.contentHash;
    state.length = state.durableOffset = manifest.length;
    Mutation mutation{&state, boundary, false, false, {}};
    auto audit = makeUniqueNoThrow<HalDictionaryZipAudit>(
        state, manifest, state.storageGeneration, "/dictionaries/imported/dictionary", compare, progress, &mutation);
    ASSERT_TRUE(audit && audit->begin());
    mutation.armed = true;
    const bool finished = audit->finish();
    if (mutation.changed) {
      EXPECT_FALSE(finished) << boundary;
      EXPECT_EQ(inventory_hal_test::state.files, mutation.saved) << boundary;
    } else {
      EXPECT_TRUE(finished) << boundary;
    }
  }
}

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <array>
#include <fstream>
#include <iterator>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalDictionaryBindings.h"
#include "lib/hal/HalDictionaryCacheStorage.h"
using namespace companion;
namespace {
class HalDictionaryBindingsTest : public testing::Test {
 protected:
  std::array<uint8_t, 256> scratch;
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings bindings{cache, scratch};
  void SetUp() override { inventory_hal_test::state = {}; }
  static std::vector<uint8_t> fixture(const char* name) {
    std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  ContentManifest archive(bool compressed, bool comment) {
    auto bytes = fixture(compressed ? "DictionaryBundle-dictzip.fixture" : "DictionaryBundle-plain.fixture");
    if (comment) {
      bytes[bytes.size() - 2] = 3;
      bytes.insert(bytes.end(), {'z', 'i', 'p'});
    }
    auto& state = inventory_hal_test::state;
    state.files[DICTIONARY_CACHE_CANDIDATE] = bytes;
    ContentManifest manifest;
    manifest.kind = ContentKind::Dictionary;
    manifest.formatVersion = 1;
    manifest.length = bytes.size();
    EVP_Digest(bytes.data(), bytes.size(), manifest.contentHash.data(), nullptr, EVP_sha256(), nullptr);
    DictionaryCachePublication publication(cache, scratch);
    EXPECT_EQ(publication.publish(manifest), DictionaryCacheResult::Ok);
    return manifest;
  }
  DictionaryArchiveBinding value(bool compressed = false) {
    return {archive(compressed, false), archive(compressed, true)};
  }
  static DictionaryRemovalPlan removalPlan(const DictionaryArchiveBinding& binding) {
    DictionaryRemovalPlan plan;
    plan.request.transaction.fill(1);
    plan.request.owner.fill(2);
    plan.request.generation.fill(3);
    plan.request.manifest = binding.original;
    auto& installed = plan.installed;
    installed.revision = 1;
    installed.phase = DictionaryInstallationPhase::Committed;
    installed.archives = binding;
    installed.extraction.revision = 1;
    installed.extraction.transaction = plan.request.transaction;
    installed.extraction.generation = plan.request.generation;
    installed.extraction.archiveHash = binding.original.contentHash;
    installed.extraction.sealed = installed.published = 7;
    installed.extraction.lengths = {100, 200, 300, 0};
    for (unsigned member = 0; member < 3; ++member) installed.extraction.hashes[member].fill(member + 5);
    std::strcpy(installed.base.data(), "/dictionaries/es/stem");
    return plan;
  }
  static std::string target(const char* path = "/dictionaries/es/stem") {
    Digest hash{};
    EVP_Digest(path, strlen(path), hash.data(), nullptr, EVP_sha256(), nullptr);
    std::string result = "/.crosspoint/companion/dictionary-binding-";
    result.reserve(110);
    constexpr char HEX[] = "0123456789abcdef";
    for (auto byte : hash) {
      result += HEX[byte >> 4];
      result += HEX[byte & 15];
    }
    return result;
  }
};
TEST_F(HalDictionaryBindingsTest, PersistedBindingSurvivesReconstructionAndRetainsOneHandle) {
  const auto expected = value();
  DictionaryArchiveBinding output;
  EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Missing);
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", expected));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", expected));
  const auto wrappers = inventory_hal_test::state.preparations;
  for (unsigned read = 0; read < 5; ++read) {
    ASSERT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
    EXPECT_EQ(output, expected);
    EXPECT_EQ(inventory_hal_test::state.preparations, wrappers);
  }
  HalDictionaryBindings rebooted(cache, scratch);
  ASSERT_EQ(rebooted.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
  EXPECT_EQ(output, expected);
  EXPECT_EQ(rebooted.read("/dictionaries/fr/stem", output), DictionaryBindingResult::Missing);
  EXPECT_EQ(output, expected);
}
TEST_F(HalDictionaryBindingsTest, ReplacementKeepsBackupAndBlocksNormalReadsUntilFinalized) {
  const auto old = value(), next = value(true);
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", old));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", old));
  const auto oldBytes = inventory_hal_test::state.files.at(target());
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
  EXPECT_EQ(inventory_hal_test::state.files.at(target() + ".bak"), oldBytes);
  auto output = old;
  EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Error);
  EXPECT_EQ(output, old);
  EXPECT_FALSE(bindings.finalizeInstallation("/dictionaries/es/stem", old));
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", next));
  ASSERT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
  EXPECT_EQ(output, next);
}
TEST_F(HalDictionaryBindingsTest, EveryRenameFailureBeforeOrAfterEffectsRetriesWithoutLosingOldBinding) {
  for (bool after : {false, true}) {
    for (unsigned mutation = 1; mutation <= 2; ++mutation) {
      SetUp();
      const auto old = value(), next = value(true);
      ASSERT_TRUE(bindings.install("/dictionaries/es/stem", old));
      ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", old));
      auto& state = inventory_hal_test::state;
      const auto oldBytes = state.files.at(target());
      if (after)
        state.failRenameAfter = state.renames + mutation;
      else
        state.failRename = state.renames + mutation;
      EXPECT_FALSE(bindings.install("/dictionaries/es/stem", next));
      EXPECT_TRUE((state.files.contains(target()) && state.files.at(target()) == oldBytes) ||
                  (state.files.contains(target() + ".bak") && state.files.at(target() + ".bak") == oldBytes));
      auto output = old;
      EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Error);
      state.failRename = state.failRenameAfter = 0;
      ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
      ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", next));
      ASSERT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
      EXPECT_EQ(output, next);
    }
  }
}
TEST_F(HalDictionaryBindingsTest, CleanupFailureBeforeOrAfterEffectsCanBeRetried) {
  for (bool after : {false, true}) {
    SetUp();
    const auto old = value(), next = value(true);
    ASSERT_TRUE(bindings.install("/dictionaries/es/stem", old));
    ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", old));
    ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
    if (after)
      inventory_hal_test::state.failRemoveAfter = true;
    else
      inventory_hal_test::state.failRemove = true;
    EXPECT_FALSE(bindings.finalizeInstallation("/dictionaries/es/stem", next));
    inventory_hal_test::state.failRemove = inventory_hal_test::state.failRemoveAfter = false;
    ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", next));
    DictionaryArchiveBinding output;
    EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
    EXPECT_EQ(output, next);
  }
}
TEST_F(HalDictionaryBindingsTest, CorruptReadCloseAndWriteFailuresPreserveExistingBinding) {
  for (unsigned failure = 0; failure < 9; ++failure) {
    SetUp();
    const auto old = value(), next = value(true);
    ASSERT_TRUE(bindings.install("/dictionaries/es/stem", old));
    ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", old));
    auto& state = inventory_hal_test::state;
    const auto oldBytes = state.files.at(target());
    if (failure == 0) state.failWrite = true;
    if (failure == 1) state.failSync = true;
    if (failure == 2) state.failTruncate = true;
    if (failure == 3) state.corruptWrite = true;
    if (failure == 4) state.failClose = true;
    if (failure == 5) state.readErrorPath = target();
    if (failure == 6) state.failOpen = true;
    if (failure == 7) state.failClosePath = target() + ".tmp";
    if (failure == 8) state.readErrorPath = target() + ".tmp";
    EXPECT_FALSE(bindings.install("/dictionaries/es/stem", next)) << failure;
    EXPECT_EQ(state.files.at(target()), oldBytes);
    state.failWrite = state.failSync = state.failTruncate = state.corruptWrite = state.failClose = state.failOpen =
        false;
    state.readErrorPath.clear();
    state.failClosePath.clear();
    ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
    ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", next));
  }
}
TEST_F(HalDictionaryBindingsTest, WrongPathCorruptRecordMissingArchivesAndInvalidArgumentsFailClosed) {
  const auto expected = value();
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", expected));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", expected));
  auto& state = inventory_hal_test::state;
  auto output = expected;
  state.files[target("/dictionaries/fr/stem")] = state.files.at(target());
  EXPECT_EQ(bindings.read("/dictionaries/fr/stem", output), DictionaryBindingResult::Error);
  EXPECT_EQ(output, expected);
  state.files[target()][10] ^= 1;
  EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Error);
  EXPECT_EQ(output, expected);
  EXPECT_FALSE(bindings.install("/dictionaries/es/stem", expected));
  EXPECT_FALSE(bindings.install("/dictionaries/../es", expected));
  EXPECT_EQ(bindings.read(nullptr, output), DictionaryBindingResult::Error);
  state.files.clear();
  EXPECT_FALSE(bindings.install("/dictionaries/es/stem", expected));
  EXPECT_TRUE(state.files.empty());
}
TEST_F(HalDictionaryBindingsTest, ReadCloseFailurePreservesOutputAndRetrySucceeds) {
  const auto expected = value();
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", expected));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", expected));
  auto output = expected;
  output.original.length = 99;
  inventory_hal_test::state.failClosePath = target();
  EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Error);
  EXPECT_EQ(output.original.length, 99u);
  inventory_hal_test::state.failClosePath.clear();
  ASSERT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
  EXPECT_EQ(output, expected);
}
TEST_F(HalDictionaryBindingsTest, VerifiedReplacementCanFinishWithDamagedOwnedBackup) {
  const auto old = value(), next = value(true);
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", old));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", old));
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
  inventory_hal_test::state.files[target() + ".bak"][0] ^= 1;
  DictionaryArchiveBinding output;
  EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Error);
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", next));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", next));
  EXPECT_FALSE(inventory_hal_test::state.files.contains(target() + ".bak"));
  ASSERT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
  EXPECT_EQ(output, next);
}
TEST_F(HalDictionaryBindingsTest, BooleanExistsCannotHideBindingAndDirectoryIoPreservesOutput) {
  const auto expected = value();
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", expected));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", expected));
  inventory_hal_test::state.falseExists = true;
  DictionaryArchiveBinding output;
  ASSERT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Found);
  EXPECT_EQ(output, expected);
  const auto files = inventory_hal_test::state.files;
  inventory_hal_test::state.directoryErrorPath = TRANSFER_DIRECTORY;
  output.original.length = 99;
  EXPECT_EQ(bindings.read("/dictionaries/es/stem", output), DictionaryBindingResult::Error);
  EXPECT_EQ(output.original.length, 99u);
  EXPECT_FALSE(bindings.install("/dictionaries/es/stem", expected));
  EXPECT_FALSE(bindings.finalizeInstallation("/dictionaries/es/stem", expected));
  EXPECT_EQ(inventory_hal_test::state.files, files);
}
}  // namespace

TEST_F(HalDictionaryBindingsTest, RemovalRequiresCommittedOwnerAndRetainsArchives) {
  const auto binding = value();
  const auto plan = removalPlan(binding);
  ASSERT_TRUE(bindings.install(plan.installed.base.data(), binding));
  ASSERT_TRUE(bindings.finalizeInstallation(plan.installed.base.data(), binding));
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal(storage, journalScratch);
  ContentRemovalRecord initial;
  initial.request = plan.request;
  initial.planHash.fill(8);
  EXPECT_FALSE(bindings.retireRemoval(journal, plan, initial.planHash));
  ASSERT_EQ(journal.begin(initial), ContentRemovalJournalResult::Ok);
  EXPECT_FALSE(bindings.retireRemoval(journal, plan, initial.planHash));
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
  EXPECT_FALSE(bindings.retireRemoval(journal, plan, initial.planHash));
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Committed), ContentRemovalJournalResult::Ok);
  auto foreign = plan;
  foreign.request.owner.fill(9);
  EXPECT_FALSE(bindings.retireRemoval(journal, foreign, initial.planHash));
  auto wrongHash = initial.planHash;
  wrongHash[0] ^= 1;
  EXPECT_FALSE(bindings.retireRemoval(journal, plan, wrongHash));
  auto& state = inventory_hal_test::state;
  const auto before = state.files;
  ASSERT_TRUE(bindings.retireRemoval(journal, plan, initial.planHash));
  auto expected = before;
  expected.erase(target());
  EXPECT_EQ(state.files, expected);
  EXPECT_TRUE(bindings.verifyRemovalRetired(journal, plan, initial.planHash));
  EXPECT_TRUE(bindings.retireRemoval(journal, plan, initial.planHash));
  EXPECT_EQ(state.files, expected);
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Retired), ContentRemovalJournalResult::Ok);
  EXPECT_TRUE(bindings.verifyRemovalRetired(journal, plan, initial.planHash));
  EXPECT_FALSE(bindings.retireRemoval(journal, plan, initial.planHash));
}
TEST_F(HalDictionaryBindingsTest, RemovalFailuresBeforeAndAfterDeletionRecoverAfterReconstruction) {
  for (bool after : {false, true}) {
    SetUp();
    const auto binding = value();
    const auto plan = removalPlan(binding);
    ASSERT_TRUE(bindings.install(plan.installed.base.data(), binding));
    ASSERT_TRUE(bindings.finalizeInstallation(plan.installed.base.data(), binding));
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
    HalContentRemovalJournalStorage storage;
    ContentRemovalJournal journal(storage, journalScratch);
    ContentRemovalRecord initial;
    initial.request = plan.request;
    initial.planHash.fill(8);
    ASSERT_EQ(journal.begin(initial), ContentRemovalJournalResult::Ok);
    ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
    ASSERT_EQ(journal.advance(ContentRemovalPhase::Committed), ContentRemovalJournalResult::Ok);
    auto& state = inventory_hal_test::state;
    state.failRemove = !after;
    state.failRemoveAfter = after;
    EXPECT_FALSE(bindings.retireRemoval(journal, plan, initial.planHash));
    EXPECT_EQ(state.files.contains(target()), !after);
    state.failRemove = state.failRemoveAfter = false;
    ContentRemovalJournal recovered(storage, journalScratch);
    ASSERT_EQ(recovered.recover(initial), ContentRemovalJournalResult::Ok);
    HalDictionaryBindings rebooted(cache, scratch);
    ASSERT_TRUE(rebooted.retireRemoval(recovered, plan, initial.planHash));
    EXPECT_TRUE(rebooted.verifyRemovalRetired(recovered, plan, initial.planHash));
  }
}
TEST_F(HalDictionaryBindingsTest, RemovalRefusesCorruptForeignOrPendingBindingsWithoutDeletion) {
  for (unsigned variant = 0; variant < 4; ++variant) {
    SetUp();
    const auto binding = value();
    const auto plan = removalPlan(binding);
    ASSERT_TRUE(bindings.install(plan.installed.base.data(), binding));
    ASSERT_TRUE(bindings.finalizeInstallation(plan.installed.base.data(), binding));
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
    HalContentRemovalJournalStorage storage;
    ContentRemovalJournal journal(storage, journalScratch);
    ContentRemovalRecord initial;
    initial.request = plan.request;
    initial.planHash.fill(8);
    ASSERT_EQ(journal.begin(initial), ContentRemovalJournalResult::Ok);
    ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
    ASSERT_EQ(journal.advance(ContentRemovalPhase::Committed), ContentRemovalJournalResult::Ok);
    auto& state = inventory_hal_test::state;
    if (variant == 0) state.files.at(target())[40] ^= 1;
    if (variant == 1) {
      const auto different = value(true);
      ASSERT_TRUE(bindings.install(plan.installed.base.data(), different));
      ASSERT_TRUE(bindings.finalizeInstallation(plan.installed.base.data(), different));
    }
    if (variant >= 2) state.files[target() + (variant == 2 ? ".tmp" : ".bak")] = {1};
    const auto before = state.files;
    EXPECT_FALSE(bindings.retireRemoval(journal, plan, initial.planHash));
    EXPECT_EQ(state.files, before);
  }
}
TEST_F(HalDictionaryBindingsTest, JournalCheckpointChangeDuringLookupPreventsBindingDeletion) {
  const auto binding = value();
  const auto plan = removalPlan(binding);
  ASSERT_TRUE(bindings.install(plan.installed.base.data(), binding));
  ASSERT_TRUE(bindings.finalizeInstallation(plan.installed.base.data(), binding));
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal(storage, journalScratch);
  ContentRemovalRecord initial;
  initial.request = plan.request;
  initial.planHash.fill(8);
  ASSERT_EQ(journal.begin(initial), ContentRemovalJournalResult::Ok);
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Quarantined), ContentRemovalJournalResult::Ok);
  ASSERT_EQ(journal.advance(ContentRemovalPhase::Committed), ContentRemovalJournalResult::Ok);
  struct Callback {
    ContentRemovalJournal* journal;
    bool changed = false;
  } callback{&journal};
  HalDictionaryBindings guarded(
      cache, scratch,
      [](void* context) {
        auto& callback = *static_cast<Callback*>(context);
        if (!callback.changed) {
          callback.changed = true;
          EXPECT_EQ(callback.journal->advance(ContentRemovalPhase::Retired), ContentRemovalJournalResult::Ok);
        }
        return true;
      },
      &callback);
  const auto bytes = inventory_hal_test::state.files.at(target());
  EXPECT_FALSE(guarded.retireRemoval(journal, plan, initial.planHash));
  EXPECT_TRUE(callback.changed);
  EXPECT_EQ(inventory_hal_test::state.files.at(target()), bytes);
}
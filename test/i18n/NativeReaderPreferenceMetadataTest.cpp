#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <fstream>
#include <iterator>

#include "CompanionBookmarkEditionStage.h"
#include "CompanionBookmarkIdentityEnumeration.h"
#include "CompanionBookmarkJsonPreparation.h"
#include "CompanionBookmarkReplay.h"
#include "CompanionBookmarkSaveSession.h"
#include "CompanionReaderPreferenceMetadata.h"
#include "CompanionReaderPreferencePhase.h"
#include "CompanionReaderPreferenceReplay.h"
#include "CompanionReaderPreferenceSaveSession.h"
#include "CompanionReaderProgressSaveCache.h"
#include "CompanionReadingPositionReplay.h"
#include "CompanionReadingPositionSaveSession.h"
#include "HalDictionaryCacheStorage.h"
#include "activities/reader/ProgressFile.h"

using namespace companion;
TEST(NativeReaderPreferenceMetadataTest, ActualRegistryAndFontProofProduceCheckedIdentityAndClearFailures) {
  inventory_hal_test::state = {};
  auto& state = inventory_hal_test::state;
  state.directories["/"] = {{".fonts", true}, {".crosspoint", true}};
  state.directories["/.fonts"] = {{"Test", true}};
  state.directories["/.fonts/Test"] = {{"Test_14.cpfont", false}};
  std::ifstream input(std::string(COMPANION_FIXTURE_DIR) + "/BitmapFont-v4.fixture", std::ios::binary);
  state.files["/.fonts/Test/Test_14.cpfont"] = {std::istreambuf_iterator<char>(input), {}};
  ASSERT_FALSE(state.files.begin()->second.empty());
  SdCardFontRegistry registry;
  ASSERT_TRUE(registry.discover());
  std::array<uint8_t, 256> scratch{};
  std::array<uint32_t, 256> decoded{};
  std::array<uint32_t, 1024> normalized{};
  std::array<uint8_t, 768> wanted{}, found{};
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings bindings(cache, scratch);
  NativeReaderPreferenceMetadata metadata(registry, bindings, scratch, decoded, normalized, wanted, found);
  ReaderPreferenceValues values;
  EXPECT_TRUE(metadata.languageTag().empty());
  ASSERT_TRUE(metadata.capture(values));
  EXPECT_EQ(metadata.languageTag(), "en");
  EXPECT_TRUE(metadata.fontHash().empty());
  values.fontPointSize = 14;
  std::copy_n("Test", 5, values.sdFontFamilyName.begin());
  Digest expected{};
  const auto& bytes = state.files.at("/.fonts/Test/Test_14.cpfont");
  ASSERT_EQ(EVP_Digest(bytes.data(), bytes.size(), expected.data(), nullptr, EVP_sha256(), nullptr), 1);
  const auto original = state.files;
  ASSERT_TRUE(metadata.capture(values));
  EXPECT_TRUE(std::equal(metadata.fontHash().begin(), metadata.fontHash().end(), expected.begin()));
  EXPECT_EQ(metadata.fontHash().size(), 32U);
  values.fontPointSize = 16;
  EXPECT_FALSE(metadata.capture(values));
  EXPECT_TRUE(metadata.languageTag().empty());
  EXPECT_TRUE(metadata.fontHash().empty());
  values.fontPointSize = 14;
  ASSERT_TRUE(metadata.capture(values));
  state.failClose = true;
  EXPECT_FALSE(metadata.capture(values));
  EXPECT_TRUE(metadata.fontHash().empty());
  state.failClose = false;
  ASSERT_TRUE(metadata.capture(values));
  values.language = 255;
  EXPECT_FALSE(metadata.capture(values));
  EXPECT_TRUE(metadata.languageTag().empty());
  EXPECT_TRUE(metadata.fontHash().empty());
  EXPECT_EQ(state.files, original);
}

TEST(NativeReaderPreferenceMetadataTest, ReplayAuditsAuthorityChecksDependenciesAndRetainsConflicts) {
  inventory_hal_test::state = {};
  inventory_hal_test::state.enumerateFileMap = true;
  SdCardFontRegistry registry;
  std::array<uint8_t, 512> scratch{};
  std::array<uint32_t, 256> decoded{};
  std::array<uint32_t, 1024> normalized{};
  std::array<uint8_t, 768> wanted{}, found{};
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings bindings(cache, scratch);
  NativeReaderPreferenceMetadata metadata(registry, bindings, scratch, decoded, normalized, wanted, found);
  HalTintaJournalStorage storage;
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = 1;
  event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  std::array<uint8_t, 8> body{1, static_cast<uint8_t>(EventKind::Preference), 8, 1, 20, 0, 0, 0};
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  ASSERT_TRUE(storage.close());
  CrossPointSettings settings;
  auto phase = makeUniqueNoThrow<NativeReaderPreferencePhase>(settings, registry, scratch);
  ASSERT_TRUE(phase);
  ASSERT_EQ(phase->apply(), ReaderPreferenceApplicationResult::Applied);
  EXPECT_EQ(settings.values.screenMargin, 20);
  EXPECT_EQ(settings.deviceOnly, 42);
  EXPECT_EQ(phase->apply(), ReaderPreferenceApplicationResult::Unchanged);
  const auto first = event.identity;
  event.identity.sequence = 2;
  event.ancestorCount = 1;
  event.ancestors[0] = first;
  body[2] = 2;
  body[4] = 18;
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  ASSERT_TRUE(storage.close());
  const auto saved = settings.values;
  EXPECT_EQ(phase->apply(), ReaderPreferenceApplicationResult::UnavailableDependency);
  EXPECT_EQ(settings.values, saved);
  event.ancestors[0] = event.identity;
  ++event.identity.sequence;
  body[4] = 16;
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  ASSERT_TRUE(storage.close());
  ASSERT_EQ(phase->apply(), ReaderPreferenceApplicationResult::Applied);
  EXPECT_EQ(settings.values.fontPointSize, 16);
  const auto applied = settings.values;
  event.identity.origin.fill(3);
  event.identity.sequence = 1;
  event.ancestorCount = 0;
  body[2] = 8;
  body[4] = 25;
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  ASSERT_TRUE(storage.close());
  EXPECT_EQ(phase->apply(), ReaderPreferenceApplicationResult::Conflict);
  EXPECT_EQ(settings.values, applied);
}

TEST(NativeReaderPreferenceMetadataTest, InstalledDictionaryIsHashedWithoutWritesAndIoClearsMetadata) {
  inventory_hal_test::state = {};
  auto& state = inventory_hal_test::state;
  state.directories["/"] = {{"dictionaries", true}, {".crosspoint", true}};
  state.directories["/.crosspoint"] = {{"companion", true}};
  state.directories["/.crosspoint/companion"] = {};
  state.directories["/dictionaries"] = {{"es", true}};
  state.directories["/dictionaries/es"] = {
      {"stem.idx", false}, {"stem.ifo", false}, {"stem.dict", false}, {"stem.syn", false}};
  const auto fixture = [](const char* name) {
    std::ifstream input(std::string(COMPANION_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return std::vector<uint8_t>{std::istreambuf_iterator<char>(input), {}};
  };
  state.files["/dictionaries/es/stem.idx"] = fixture("DictionaryIndex-definitions.fixture");
  state.files["/dictionaries/es/stem.syn"] = fixture("DictionaryIndex-synonyms.fixture");
  state.files["/dictionaries/es/stem.dict"] = {'o', 'n', 'e', 't', 'w', 'o'};
  const std::string info =
      "StarDict's dict ifo file\nversion=3.0.0\nbookname=Canonical "
      "dictionary\nwordcount=2\nidxfilesize=24\nsynwordcount=1\n";
  state.files["/dictionaries/es/stem.ifo"] = {info.begin(), info.end()};
  SdCardFontRegistry registry;
  std::array<uint8_t, 256> scratch{};
  std::array<uint32_t, 256> decoded{};
  std::array<uint32_t, 1024> normalized{};
  std::array<uint8_t, 768> wanted{}, found{};
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings bindings(cache, scratch);
  NativeReaderPreferenceMetadata metadata(registry, bindings, scratch, decoded, normalized, wanted, found);
  ReaderPreferenceValues values;
  std::copy_n("es", 3, values.dictionaryName.begin());
  const auto original = state.files;
  ASSERT_TRUE(metadata.capture(values));
  const auto canonical = fixture("DictionaryBundle-plain.fixture");
  Digest expected{};
  ASSERT_EQ(EVP_Digest(canonical.data(), canonical.size(), expected.data(), nullptr, EVP_sha256(), nullptr), 1);
  ASSERT_EQ(metadata.dictionaryHash().size(), 32U);
  EXPECT_TRUE(std::equal(metadata.dictionaryHash().begin(), metadata.dictionaryHash().end(), expected.begin()));
  state.directoryErrorPath = "/dictionaries";
  EXPECT_FALSE(metadata.capture(values));
  EXPECT_TRUE(metadata.dictionaryHash().empty());
  EXPECT_TRUE(metadata.languageTag().empty());
  state.directoryErrorPath.clear();
  ASSERT_TRUE(metadata.capture(values));
  values.dictionaryName.fill('x');
  EXPECT_FALSE(metadata.capture(values));
  EXPECT_TRUE(metadata.dictionaryHash().empty());
  EXPECT_EQ(state.files, original);
  std::array<uint8_t, 69> body{};
  EXPECT_EQ(metadata.encodePreference(values, 8, body), 8U);
  EXPECT_EQ(metadata.encodePreference(values, 11, body), 0U);
  values.dictionaryName = {};
  EXPECT_EQ(metadata.encodePreference(values, 11, body), 5U);
}

namespace {
class SaveIdentities final : public IdentityStorage {
 public:
  unsigned calls = 0;
  uint8_t random = 8;
  bool hardwareIdentity(Identity& output) override {
    ++calls;
    output.fill(9);
    return true;
  }
  bool cardIdentity(Identity& output) override {
    ++calls;
    output.fill(2);
    return true;
  }
  IdentityRead readBinding(std::span<uint8_t>) override {
    ++calls;
    return IdentityRead::Missing;
  }
  bool writeBinding(std::span<const uint8_t>) override {
    ++calls;
    return true;
  }
  IdentityRead readMarker(Identity&) override {
    ++calls;
    return IdentityRead::Missing;
  }
  bool createMarker(const Identity&) override {
    ++calls;
    return true;
  }
  bool randomIdentity(Identity& output) override {
    ++calls;
    output.fill(random++);
    return true;
  }
};
}  // namespace
TEST(NativeReaderPreferenceMetadataTest, SaveSessionPreflightsThenResolvesEveryObservedBranchAndReplays) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  SdCardFontRegistry registry;
  std::array<uint8_t, 512> scratch{};
  HalTintaJournalStorage storage;
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  SyncEvent event;
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  std::array<uint8_t, 8> body{1, 4, 8, 1, 20, 0, 0, 0};
  for (uint8_t origin = 1; origin <= 6; ++origin) {
    event.identity.origin.fill(origin);
    body[4] = origin * 5;
    ASSERT_TRUE(storage.digest(body, event.bodyHash));
    ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  }
  ASSERT_TRUE(storage.close());
  ReaderPreferenceValues baseline, candidate;
  baseline.screenMargin = 5;
  candidate = baseline;
  candidate.screenMargin = 40;
  SaveIdentities identities;
  auto save = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(baseline, candidate, identities), TintaJournalResult::Ok);
  EXPECT_FALSE(save->requiresRecovery());
  save.reset();
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(journal.count(), 8U);
  ASSERT_TRUE(storage.close());
  CrossPointSettings settings;
  settings.values = baseline;
  auto replay = makeUniqueNoThrow<NativeReaderPreferencePhase>(settings, registry, scratch);
  ASSERT_TRUE(replay);
  ASSERT_EQ(replay->apply(), ReaderPreferenceApplicationResult::Applied);
  EXPECT_EQ(settings.values, candidate);
  EXPECT_EQ(settings.deviceOnly, 42);
}
TEST(NativeReaderPreferenceMetadataTest, InvalidOrUnchangedSaveDoesNotReserveIdentityOrOpenJournal) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  SdCardFontRegistry registry;
  std::array<uint8_t, 512> scratch{};
  ReaderPreferenceValues baseline, candidate;
  candidate = baseline;
  std::copy_n("Missing", 8, candidate.sdFontFamilyName.begin());
  SaveIdentities identities;
  auto save = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(save);
  const auto before = state.files;
  EXPECT_EQ(save->persist(baseline, candidate, identities), TintaJournalResult::Invalid);
  EXPECT_FALSE(save->requiresRecovery());
  EXPECT_EQ(identities.calls, 0U);
  EXPECT_EQ(state.files, before);
  save.reset();
  save = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(save);
  EXPECT_EQ(save->persist(baseline, baseline, identities), TintaJournalResult::Ok);
  EXPECT_EQ(identities.calls, 0U);
  EXPECT_EQ(state.files, before);
}

TEST(NativeReaderPreferenceMetadataTest, InitialImportPreservesExistingKeysAndIsIdempotent) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  SdCardFontRegistry registry;
  std::array<uint8_t, 512> scratch{};
  HalTintaJournalStorage storage;
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::Preference;
  event.resource = PREFERENCE_SCOPE;
  const std::array<uint8_t, 8> body{1, 4, 8, 1, 20, 0, 0, 0};
  ASSERT_TRUE(storage.digest(body, event.bodyHash));
  ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  ASSERT_TRUE(storage.close());
  ReaderPreferenceValues values;
  values.screenMargin = 40;
  SaveIdentities identities;
  auto importer = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(importer);
  ASSERT_EQ(importer->importMissing(values, identities), TintaJournalResult::Ok);
  importer.reset();
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(journal.count(), 14u);
  ASSERT_TRUE(storage.close());
  const auto calls = identities.calls;
  importer = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(importer);
  EXPECT_EQ(importer->importMissing(values, identities), TintaJournalResult::Ok);
  EXPECT_EQ(identities.calls, calls);
  importer.reset();
  CrossPointSettings settings;
  settings.values = values;
  auto replay = makeUniqueNoThrow<NativeReaderPreferencePhase>(settings, registry, scratch);
  ASSERT_TRUE(replay);
  ASSERT_EQ(replay->apply(), ReaderPreferenceApplicationResult::Applied);
  EXPECT_EQ(settings.values.screenMargin, 20);
  EXPECT_EQ(settings.deviceOnly, 42);
}

TEST(NativeReaderPreferenceMetadataTest, InitialImportRefusesConflictOrMissingContentBeforeIdentityReservation) {
  for (const bool conflicted : {false, true}) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    SdCardFontRegistry registry;
    std::array<uint8_t, 512> scratch{};
    HalTintaJournalStorage storage;
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    if (conflicted) {
      SyncEvent event;
      event.identity.epoch = event.identity.sequence = 1;
      event.storageGeneration.fill(2);
      event.kind = EventKind::Preference;
      event.resource = PREFERENCE_SCOPE;
      std::array<uint8_t, 8> body{1, 4, 8, 1, 20, 0, 0, 0};
      for (uint8_t origin = 1; origin <= 2; ++origin) {
        event.identity.origin.fill(origin);
        body[4] = origin * 10;
        ASSERT_TRUE(storage.digest(body, event.bodyHash));
        ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
      }
    }
    ASSERT_TRUE(storage.close());
    const auto active = state.files.at(TINTA_JOURNAL_EVENTS);
    ReaderPreferenceValues values;
    std::copy_n("Missing", 8, values.sdFontFamilyName.begin());
    SaveIdentities identities;
    auto importer = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
    ASSERT_TRUE(importer);
    EXPECT_EQ(importer->importMissing(values, identities),
              conflicted ? TintaJournalResult::Conflict : TintaJournalResult::Invalid);
    EXPECT_EQ(identities.calls, 0u);
    EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), active);
    EXPECT_FALSE(importer->requiresRecovery());
  }
}

TEST(NativeReaderPreferenceMetadataTest, InterruptedInitialImportResumesOnlyMissingKeys) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  SdCardFontRegistry registry;
  std::array<uint8_t, 1024> scratch{};
  ReaderPreferenceValues values;
  SaveIdentities identities;
  state.failWritePath = TINTA_JOURNAL_EVENTS;
  state.failMatchingWrite = 2;
  auto importer = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(importer);
  EXPECT_EQ(importer->importMissing(values, identities), TintaJournalResult::IoError);
  EXPECT_TRUE(importer->requiresRecovery());
  importer.reset();
  state.failWritePath.clear();
  HalTintaJournalStorage storage;
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(journal.count(), 1u);
  ASSERT_EQ(journal.read(0), TintaJournalResult::Ok);
  const auto first = journal.event().identity;
  ASSERT_TRUE(storage.close());
  importer = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(importer);
  ASSERT_EQ(importer->importMissing(values, identities), TintaJournalResult::Ok);
  EXPECT_FALSE(importer->requiresRecovery());
  importer.reset();
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(journal.count(), 14u);
  ASSERT_EQ(journal.read(0), TintaJournalResult::Ok);
  EXPECT_EQ(journal.event().identity, first);
  ASSERT_TRUE(storage.close());
  HalJournalCausalAuditSession audit;
  PortablePreferenceResolution resolution;
  ASSERT_TRUE(audit.run());
  ASSERT_EQ(audit.resolvePortablePreferences(resolution), TintaJournalResult::Ok);
  uint16_t missing = 0xffff;
  ASSERT_TRUE(resolution.missingReaderKeys(missing));
  EXPECT_EQ(missing, 0u);
}

TEST(NativeReaderPreferenceMetadataTest, ReadingSaveUsesAuditedHeadsAndRetainsExactAnchor) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  SaveIdentities identities;
  auto save = makeUniqueNoThrow<NativeReadingPositionSaveSession>();
  ASSERT_TRUE(save);
  Digest edition{};
  edition.fill(7);
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  SyncEvent event;
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::ReadingPosition;
  event.resource = edition;
  std::array<uint8_t, 8> body{1, 1, 0, 0, 20, 0, 0, 0};
  for (uint8_t origin = 1; origin <= 6; ++origin) {
    event.identity.origin.fill(origin);
    body[4] = origin * 5;
    ASSERT_TRUE(storage.digest(body, event.bodyHash));
    ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  }
  ASSERT_TRUE(storage.close());
  const ReadingAnchor anchor{65535, UINT32_MAX};
  const auto before = state.files.at(TINTA_JOURNAL_EVENTS);
  EXPECT_EQ(save->persist(anchor, edition, identities), TintaJournalResult::Conflict);
  EXPECT_EQ(identities.calls, 0u);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), before);
  EXPECT_FALSE(save->requiresRecovery());
  save.reset();
  save = makeUniqueNoThrow<NativeReadingPositionSaveSession>();
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(anchor, edition, identities, true), TintaJournalResult::Ok);
  EXPECT_FALSE(save->requiresRecovery());
  EXPECT_EQ(save->persist(anchor, edition, identities), TintaJournalResult::Unavailable);
  save.reset();
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(journal.count(), 8u);
  ASSERT_EQ(journal.read(6), TintaJournalResult::Ok);
  EXPECT_EQ(journal.event().ancestorCount, 4);
  const auto firstBatch = journal.event().identity;
  ASSERT_EQ(journal.read(7), TintaJournalResult::Ok);
  EXPECT_EQ(journal.event().ancestorCount, 2);
  EXPECT_EQ(journal.event().identity.origin, firstBatch.origin);
  EXPECT_EQ(journal.event().identity.epoch, firstBatch.epoch);
  EXPECT_EQ(journal.event().identity.sequence, firstBatch.sequence + 1);
  ReadingAnchor decoded;
  ASSERT_TRUE(decodeReadingAnchor(journal.body(), decoded));
  EXPECT_EQ(decoded, anchor);
  EXPECT_EQ(journal.event().resource, edition);
  ASSERT_TRUE(storage.close());
  HalJournalCausalAuditSession audit;
  ReadingAnchor resolved{9, 99};
  EXPECT_EQ(audit.resolveReadingPosition(edition, resolved), TintaJournalResult::Unavailable);
  EXPECT_EQ(resolved, (ReadingAnchor{9, 99}));
  ASSERT_TRUE(audit.run());
  ASSERT_EQ(audit.resolveReadingPosition(edition, resolved), TintaJournalResult::Ok);
  EXPECT_EQ(resolved, anchor);
  const auto identityCalls = identities.calls;
  save = makeUniqueNoThrow<NativeReadingPositionSaveSession>();
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(anchor, edition, identities), TintaJournalResult::Ok);
  EXPECT_EQ(identities.calls, identityCalls);
  save.reset();
  ASSERT_TRUE(audit.run());
  EXPECT_EQ(audit.recordCount(), 8u);
  ASSERT_TRUE(audit.run());
  state.failClosePath = TINTA_JOURNAL_EVENTS;
  resolved = {9, 99};
  EXPECT_EQ(audit.resolveReadingPosition(edition, resolved), TintaJournalResult::IoError);
  EXPECT_EQ(resolved, (ReadingAnchor{9, 99}));
  state.failClosePath.clear();
  EXPECT_EQ(audit.resolveReadingPosition(edition, resolved), TintaJournalResult::Unavailable);
}
TEST(NativeReaderPreferenceMetadataTest, ReadingSaveRejectsEmptyEditionBeforeJournalOrIdentityWrites) {
  auto& state = inventory_hal_test::state;
  state = {};
  SaveIdentities identities;
  auto save = makeUniqueNoThrow<NativeReadingPositionSaveSession>();
  ASSERT_TRUE(save);
  EXPECT_EQ(save->persist({}, {}, identities), TintaJournalResult::Invalid);
  EXPECT_FALSE(save->requiresRecovery());
  EXPECT_EQ(identities.calls, 0u);
  EXPECT_TRUE(state.files.empty());
}

TEST(NativeReaderPreferenceMetadataTest, ReadingSaveRefusesAuditCloseFailureBeforeIdentityReservation) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  state.failClosePath = TINTA_JOURNAL_EVENTS;
  SaveIdentities identities;
  Digest edition{};
  edition.fill(7);
  auto save = makeUniqueNoThrow<NativeReadingPositionSaveSession>();
  ASSERT_TRUE(save);
  EXPECT_EQ(save->persist({}, edition, identities), TintaJournalResult::IoError);
  EXPECT_TRUE(save->requiresRecovery());
  EXPECT_EQ(identities.calls, 0u);
  EXPECT_TRUE(state.files.at(TINTA_JOURNAL_EVENTS).empty());
}

TEST(NativeReaderPreferenceMetadataTest, ReadingSaveHashesBookAndRefusesMissingOrFailedClose) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    if (mode != 1) state.files["/book.epub"] = {1, 2, 3, 4};
    if (mode == 2) state.failClosePath = "/book.epub";
    SaveIdentities identities;
    std::array<uint8_t, 1024> scratch{};
    auto save = makeUniqueNoThrow<NativeReadingPositionSaveSession>();
    ASSERT_TRUE(save);
    const auto result = save->persistFile("/book.epub", {3, 42}, scratch, identities);
    EXPECT_EQ(result, mode == 0 ? TintaJournalResult::Ok : TintaJournalResult::IoError);
    if (mode == 1) {
      EXPECT_EQ(identities.calls, 0u);
      EXPECT_FALSE(state.files.contains(TINTA_JOURNAL_EVENTS));
      continue;
    }
    EXPECT_EQ(save->requiresRecovery(), mode == 2);
    save.reset();
    HalTintaJournalStorage storage;
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    ASSERT_EQ(journal.count(), 1u);
    ASSERT_EQ(journal.read(0), TintaJournalResult::Ok);
    Digest expected{};
    const auto& bytes = state.files.at("/book.epub");
    ASSERT_EQ(EVP_Digest(bytes.data(), bytes.size(), expected.data(), nullptr, EVP_sha256(), nullptr), 1);
    EXPECT_EQ(journal.event().resource, expected);
  }
}

TEST(NativeReaderPreferenceMetadataTest, ProgressCacheSyncFailurePreservesCanonicalPosition) {
  auto& state = inventory_hal_test::state;
  state = {};
  const std::vector<uint8_t> original{1, 0, 2, 0, 3, 0};
  state.files["/cache/progress.bin"] = original;
  const std::array<uint8_t, 10> replacement{4, 0, 0, 0, 0, 0, 42, 0, 0, 0};
  state.failSyncPath = "/cache/progress.bin.tmp";
  EXPECT_FALSE(ProgressFile::writeAtomic("/cache", replacement.data(), replacement.size()));
  EXPECT_EQ(state.files.at("/cache/progress.bin"), original);
  state.failSyncPath.clear();
  ASSERT_TRUE(ProgressFile::writeAtomic("/cache", replacement.data(), replacement.size()));
  EXPECT_EQ(state.files.at("/cache/progress.bin"), (std::vector<uint8_t>(replacement.begin(), replacement.end())));
}

TEST(NativeReaderPreferenceMetadataTest, ProgressCacheCloseFailurePreservesCanonicalPosition) {
  auto& state = inventory_hal_test::state;
  state = {};
  const std::vector<uint8_t> original{1, 0, 2, 0, 3, 0};
  state.files["/cache/progress.bin"] = original;
  const std::array<uint8_t, 10> replacement{4, 0, 0, 0, 0, 0, 42, 0, 0, 0};
  state.failClosePath = "/cache/progress.bin.tmp";
  EXPECT_FALSE(ProgressFile::writeAtomic("/cache", replacement.data(), replacement.size()));
  EXPECT_EQ(state.files.at("/cache/progress.bin"), original);
  state.failClosePath.clear();
  ASSERT_TRUE(ProgressFile::writeAtomic("/cache", replacement.data(), replacement.size()));
  EXPECT_EQ(state.files.at("/cache/progress.bin"), (std::vector<uint8_t>(replacement.begin(), replacement.end())));
}

TEST(NativeReaderPreferenceMetadataTest, ReadingReplayPublishesCheckedProgressAndRefusesFailedSync) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  Digest edition{};
  edition.fill(7);
  SaveIdentities identities;
  auto save = makeUniqueNoThrow<NativeReadingPositionSaveSession>();
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist({3, 42}, edition, identities), TintaJournalResult::Ok);
  save.reset();
  ASSERT_TRUE(Storage.ensureDirectoryExists("/cache"));
  const auto publisher = [](void*, const ReadingAnchor& anchor) {
    std::array<uint8_t, 10> bytes{};
    bytes[0] = anchor.spine & 0xff;
    bytes[1] = anchor.spine >> 8;
    for (unsigned at = 0; at < 4; ++at) bytes[6 + at] = anchor.visibleTextOffset >> (8 * at);
    return ProgressFile::writeAtomic("/cache", bytes.data(), bytes.size());
  };
  NativeReadingPositionReplay replay;
  ASSERT_EQ(replay.run(edition, 4, publisher, nullptr), TintaJournalResult::Ok);
  const auto original = state.files.at("/cache/progress.bin");
  EXPECT_EQ(original, (std::vector<uint8_t>{3, 0, 0, 0, 0, 0, 42, 0, 0, 0}));
  unsigned calls = 0;
  const auto shouldNotPublish = [](void* context, const ReadingAnchor&) {
    ++*static_cast<unsigned*>(context);
    return true;
  };
  EXPECT_EQ(replay.run(edition, 3, shouldNotPublish, &calls), TintaJournalResult::Invalid);
  EXPECT_EQ(replay.run(edition, 0, shouldNotPublish, &calls), TintaJournalResult::Invalid);
  EXPECT_EQ(replay.run(edition, 0x10001, shouldNotPublish, &calls), TintaJournalResult::Invalid);
  EXPECT_EQ(calls, 0u);
  EXPECT_EQ(state.files.at("/cache/progress.bin"), original);
  state.failSyncPath = "/cache/progress.bin.tmp";
  EXPECT_EQ(replay.run(edition, 4, publisher, nullptr), TintaJournalResult::IoError);
  EXPECT_EQ(state.files.at("/cache/progress.bin"), original);
}

TEST(NativeReaderPreferenceMetadataTest, ReadingReplayNeverPublishesMissingConflictedOrUnreadableAuthority) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    Digest edition{};
    edition.fill(7);
    HalTintaJournalStorage storage;
    std::array<uint8_t, 1024> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    if (mode != 0) {
      SyncEvent event;
      event.identity.epoch = event.identity.sequence = 1;
      event.storageGeneration.fill(2);
      event.kind = EventKind::ReadingPosition;
      event.resource = edition;
      std::array<uint8_t, 8> body{1, 1, 0, 0, 10, 0, 0, 0};
      for (uint8_t origin = 1; origin <= 2; ++origin) {
        event.identity.origin.fill(origin);
        body[4] = origin * 10;
        ASSERT_TRUE(storage.digest(body, event.bodyHash));
        ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
      }
    }
    ASSERT_TRUE(storage.close());
    state.files["/cache/progress.bin"] = {9, 0, 1, 0, 2, 0};
    const auto progress = state.files.at("/cache/progress.bin");
    const auto events = state.files.at(TINTA_JOURNAL_EVENTS);
    if (mode == 2) state.readErrorPath = TINTA_JOURNAL_EVENTS;
    unsigned calls = 0;
    const auto publisher = [](void* context, const ReadingAnchor&) {
      ++*static_cast<unsigned*>(context);
      return true;
    };
    NativeReadingPositionReplay replay;
    const auto expected = mode == 0   ? TintaJournalResult::Unavailable
                          : mode == 1 ? TintaJournalResult::Conflict
                                      : TintaJournalResult::IoError;
    EXPECT_EQ(replay.run(edition, 4, publisher, &calls), expected);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(state.files.at("/cache/progress.bin"), progress);
    EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), events);
  }
}

TEST(NativeReaderPreferenceMetadataTest, ProgressSaveCacheRequiresExactCheckedPublicationAndCurrentRevision) {
  ReaderProgressSaveCache cache;
  std::array<uint8_t, 10> bytes{1, 0, 0, 0, 0, 0, 20, 0, 0, 0};
  EXPECT_FALSE(cache.matches(1, bytes));
  ASSERT_TRUE(cache.remember(1, bytes));
  EXPECT_TRUE(cache.matches(1, bytes));
  EXPECT_FALSE(cache.matches(2, bytes));
  bytes[6] = 30;
  EXPECT_FALSE(cache.matches(1, bytes));
  EXPECT_FALSE(cache.remember(2, std::span(bytes).first(6)));
  EXPECT_FALSE(cache.matches(1, bytes));
  ASSERT_TRUE(cache.remember(2, bytes));
  cache.clear();
  EXPECT_FALSE(cache.matches(2, bytes));
}

TEST(NativeReaderPreferenceMetadataTest, BookmarkSaveRejectsInvalidInputBeforeJournalOrIdentity) {
  auto& state = inventory_hal_test::state;
  state = {};
  SaveIdentities identities;
  BookmarkBodyView bookmark;
  Digest edition{};
  edition.fill(7);
  auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
  ASSERT_TRUE(save);
  EXPECT_EQ(save->persist(bookmark, edition, identities), TintaJournalResult::Invalid);
  EXPECT_FALSE(save->requiresRecovery());
  EXPECT_EQ(identities.calls, 0u);
  EXPECT_TRUE(state.files.empty());
  EXPECT_EQ(state.opens, 0u);
  bookmark.identity[0] = 3;
  save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
  ASSERT_TRUE(save);
  EXPECT_EQ(save->persist(bookmark, {}, identities), TintaJournalResult::Invalid);
  EXPECT_EQ(identities.calls, 0u);
  EXPECT_TRUE(state.files.empty());
}

TEST(NativeReaderPreferenceMetadataTest, BookmarkPutAndDeletePersistAndUnchangedRetriesReserveNothing) {
  for (const bool deleted : {false, true}) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    SaveIdentities identities;
    BookmarkBodyView bookmark;
    bookmark.identity[0] = 3;
    bookmark.deleted = deleted;
    bookmark.anchor = {2, 99};
    Digest edition{};
    edition.fill(7);
    auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
    ASSERT_TRUE(save);
    ASSERT_EQ(save->persist(bookmark, edition, identities), TintaJournalResult::Ok);
    EXPECT_FALSE(save->requiresRecovery());
    HalJournalCausalAuditSession audit;
    ASSERT_TRUE(audit.run());
    std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
    size_t length = 0;
    ASSERT_EQ(audit.resolveBookmark(edition, bookmark.identity, body, length), TintaJournalResult::Ok);
    BookmarkBodyView decoded;
    ASSERT_TRUE(decodeBookmarkBody(std::span(body).first(length), decoded));
    EXPECT_EQ(decoded.deleted, deleted);
    EXPECT_EQ(decoded.identity, bookmark.identity);
    if (!deleted) EXPECT_EQ(decoded.anchor, bookmark.anchor);
    const auto calls = identities.calls;
    const auto active = state.files.at(TINTA_JOURNAL_EVENTS);
    save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
    ASSERT_TRUE(save);
    EXPECT_EQ(save->persist(bookmark, edition, identities), TintaJournalResult::Ok);
    EXPECT_EQ(identities.calls, calls);
    EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), active);
  }
}

TEST(NativeReaderPreferenceMetadataTest, BookmarkSaveRefusesConcurrentEditsUntilExplicitChoiceJoinsAllHeads) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  SaveIdentities identities;
  Digest edition{};
  edition.fill(7);
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  SyncEvent event;
  event.identity.epoch = event.identity.sequence = 1;
  event.storageGeneration.fill(2);
  event.kind = EventKind::BookmarkPut;
  event.resource = edition;
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 3;
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  for (uint8_t origin = 1; origin <= 6; ++origin) {
    event.identity.origin.fill(origin);
    bookmark.anchor.visibleTextOffset = origin * 5;
    const auto size = encodeBookmarkBody(bookmark, body);
    ASSERT_TRUE(storage.digest(std::span(body).first(size), event.bodyHash));
    ASSERT_EQ(journal.append(event, std::span(body).first(size)), TintaJournalResult::Ok);
  }
  ASSERT_TRUE(storage.close());
  const auto before = state.files.at(TINTA_JOURNAL_EVENTS);
  bookmark.deleted = true;
  auto replay = makeUniqueNoThrow<NativeBookmarkReplay>();
  ASSERT_TRUE(replay);
  unsigned publications = 0;
  const auto publisher = [](void* context, std::span<const uint8_t>) {
    ++*static_cast<unsigned*>(context);
    return true;
  };
  EXPECT_EQ(replay->run(edition, bookmark.identity, 4, publisher, &publications), TintaJournalResult::Conflict);
  EXPECT_EQ(publications, 0u);
  auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
  ASSERT_TRUE(save);
  EXPECT_EQ(save->persist(bookmark, edition, identities), TintaJournalResult::Conflict);
  EXPECT_FALSE(save->requiresRecovery());
  EXPECT_EQ(identities.calls, 0u);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), before);
  save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(bookmark, edition, identities, true), TintaJournalResult::Ok);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  EXPECT_EQ(journal.count(), 8u);
  ASSERT_TRUE(storage.close());
  HalJournalCausalAuditSession audit;
  ASSERT_TRUE(audit.run());
  size_t length = 0;
  ASSERT_EQ(audit.resolveBookmark(edition, bookmark.identity, body, length), TintaJournalResult::Ok);
  BookmarkBodyView decoded;
  ASSERT_TRUE(decodeBookmarkBody(std::span(body).first(length), decoded));
  EXPECT_TRUE(decoded.deleted);
  EXPECT_EQ(decoded.identity, bookmark.identity);
  EXPECT_EQ(replay->run(edition, bookmark.identity, 4, publisher, &publications), TintaJournalResult::Ok);
  EXPECT_EQ(publications, 1u);
}

TEST(NativeReaderPreferenceMetadataTest, BookmarkSaveStopsOnAuditCloseFailureBeforeIdentityReservation) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  SaveIdentities identities;
  Digest edition{};
  edition.fill(7);
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 3;
  auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(bookmark, edition, identities), TintaJournalResult::Ok);
  const auto calls = identities.calls;
  const auto before = state.files.at(TINTA_JOURNAL_EVENTS);
  ++bookmark.anchor.visibleTextOffset;
  state.failClose = true;
  save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
  ASSERT_TRUE(save);
  EXPECT_EQ(save->persist(bookmark, edition, identities), TintaJournalResult::IoError);
  EXPECT_TRUE(save->requiresRecovery());
  EXPECT_EQ(identities.calls, calls);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), before);
  state.failClose = false;
}

TEST(NativeReaderPreferenceMetadataTest, BookmarkReplayPublishesCanonicalPutAndDeleteAndPropagatesFailure) {
  for (const bool deleted : {false, true}) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    SaveIdentities identities;
    Digest edition{};
    edition.fill(7);
    BookmarkBodyView bookmark;
    bookmark.identity[0] = 3;
    bookmark.deleted = deleted;
    bookmark.anchor = {3, 42};
    auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
    ASSERT_TRUE(save);
    ASSERT_EQ(save->persist(bookmark, edition, identities), TintaJournalResult::Ok);
    save.reset();
    struct Published {
      unsigned calls = 0;
      bool succeed = true;
      std::vector<uint8_t> bytes;
    } published;
    const auto publisher = [](void* context, std::span<const uint8_t> body) {
      auto& state = *static_cast<Published*>(context);
      ++state.calls;
      state.bytes.assign(body.begin(), body.end());
      return state.succeed;
    };
    auto replay = makeUniqueNoThrow<NativeBookmarkReplay>();
    ASSERT_TRUE(replay);
    ASSERT_EQ(replay->run(edition, bookmark.identity, 4, publisher, &published), TintaJournalResult::Ok);
    EXPECT_EQ(published.calls, 1u);
    BookmarkBodyView decoded;
    ASSERT_TRUE(decodeBookmarkBody(published.bytes, decoded));
    EXPECT_EQ(decoded.deleted, deleted);
    EXPECT_EQ(decoded.identity, bookmark.identity);
    if (!deleted) EXPECT_EQ(decoded.anchor, bookmark.anchor);
    published.succeed = false;
    EXPECT_EQ(replay->run(edition, bookmark.identity, 4, publisher, &published), TintaJournalResult::IoError);
    EXPECT_EQ(published.calls, 2u);
    if (!deleted) {
      EXPECT_EQ(replay->run(edition, bookmark.identity, 3, publisher, &published), TintaJournalResult::Invalid);
      EXPECT_EQ(published.calls, 2u);
    }
    state.failClose = true;
    EXPECT_EQ(replay->run(edition, bookmark.identity, 4, publisher, &published), TintaJournalResult::IoError);
    EXPECT_EQ(published.calls, 2u);
    state.failClose = false;
    Identity missing{};
    missing[0] = 9;
    EXPECT_EQ(replay->run(edition, missing, 4, publisher, &published), TintaJournalResult::Unavailable);
    EXPECT_EQ(published.calls, 2u);
  }
}

TEST(NativeReaderPreferenceMetadataTest, BookmarkEnumerationSpoolsEditionIdsAndRetainsAuthorityBinding) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  HalTintaJournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  SyncEvent event;
  event.identity.origin.fill(1);
  event.identity.epoch = 1;
  event.storageGeneration.fill(2);
  event.resource.fill(7);
  BookmarkBodyView bookmark;
  bookmark.deleted = true;
  std::array<uint8_t, 18> body{};
  for (unsigned at = 0; at < 2; ++at) {
    bookmark.identity[0] = at ? 3 : 7;
    event.identity.sequence = at + 1;
    event.kind = EventKind::BookmarkDelete;
    ASSERT_EQ(encodeBookmarkBody(bookmark, body), body.size());
    ASSERT_TRUE(storage.digest(body, event.bodyHash));
    ASSERT_EQ(journal.append(event, body), TintaJournalResult::Ok);
  }
  ASSERT_TRUE(storage.close());
  auto enumeration = makeUniqueNoThrow<NativeBookmarkIdentityEnumeration>(scratch);
  ASSERT_TRUE(enumeration);
  ASSERT_EQ(enumeration->prepare(event.resource), TintaJournalResult::Ok);
  EXPECT_EQ(enumeration->size(), 2u);
  EXPECT_EQ(enumeration->recordCount(), 2u);
  EXPECT_EQ(enumeration->recordSize(), 1024u);
  Identity output{};
  ASSERT_EQ(enumeration->next(output), BookmarkCursorResult::Found);
  EXPECT_EQ(output[0], 3);
  ASSERT_EQ(enumeration->next(output), BookmarkCursorResult::Found);
  EXPECT_EQ(output[0], 7);
  ASSERT_EQ(enumeration->next(output), BookmarkCursorResult::End);
  HalJournalCausalAuditSession audit;
  Digest frontier{};
  ASSERT_TRUE(audit.run(&frontier));
  EXPECT_EQ(enumeration->authorityFrontier(), frontier);
  EXPECT_EQ(enumeration->prepare(event.resource), TintaJournalResult::Unavailable);
  EXPECT_TRUE(enumeration->cleanup());
  EXPECT_FALSE(state.files.count(HalBookmarkIdentityStage::PATH));
}

TEST(NativeReaderPreferenceMetadataTest, BookmarkEnumerationRefusesInvalidEditionAndForeignStage) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  std::array<uint8_t, 128> scratch{};
  auto enumeration = makeUniqueNoThrow<NativeBookmarkIdentityEnumeration>(scratch);
  ASSERT_TRUE(enumeration);
  EXPECT_EQ(enumeration->prepare({}), TintaJournalResult::Invalid);
  EXPECT_EQ(state.opens, 0u);
  EXPECT_TRUE(state.files.empty());
  state.files[HalBookmarkIdentityStage::PATH] = {1, 2, 3};
  const auto original = state.files.at(HalBookmarkIdentityStage::PATH);
  enumeration = makeUniqueNoThrow<NativeBookmarkIdentityEnumeration>(scratch);
  ASSERT_TRUE(enumeration);
  Digest edition{};
  edition.fill(7);
  EXPECT_EQ(enumeration->prepare(edition), TintaJournalResult::IoError);
  enumeration.reset();
  EXPECT_EQ(state.files.at(HalBookmarkIdentityStage::PATH), original);
}

TEST(NativeReaderPreferenceMetadataTest, EditionStageResolvesAllBookmarkBodiesBeforeExposingOutput) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    state.files["/bookmarks.json"] = {'o', 'l', 'd'};
    const auto original = state.files.at("/bookmarks.json");
    HalTintaJournalStorage storage;
    std::array<uint8_t, 1024> scratch{};
    TintaJournal journal(storage, scratch);
    ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
    SyncEvent event;
    event.identity.origin.fill(1);
    event.identity.epoch = 1;
    event.storageGeneration.fill(2);
    event.resource.fill(7);
    BookmarkBodyView bookmark;
    std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
    for (unsigned at = 0; at < 2; ++at) {
      bookmark.identity[0] = at ? 7 : 3;
      bookmark.deleted = mode == 0 && at == 1;
      bookmark.anchor = {static_cast<uint16_t>(mode == 2 && at == 1 ? 99 : 2), 42};
      event.identity.sequence = at + 1;
      event.kind = bookmark.deleted ? EventKind::BookmarkDelete : EventKind::BookmarkPut;
      const auto length = encodeBookmarkBody(bookmark, body);
      ASSERT_TRUE(storage.digest(std::span(body).first(length), event.bodyHash));
      ASSERT_EQ(journal.append(event, std::span(body).first(length)), TintaJournalResult::Ok);
    }
    if (mode == 1) {
      event.identity.origin.fill(2);
      event.identity.sequence = 1;
      bookmark.anchor.visibleTextOffset = 99;
      const auto length = encodeBookmarkBody(bookmark, body);
      ASSERT_TRUE(storage.digest(std::span(body).first(length), event.bodyHash));
      ASSERT_EQ(journal.append(event, std::span(body).first(length)), TintaJournalResult::Ok);
    }
    ASSERT_TRUE(storage.close());
    auto stage = makeUniqueNoThrow<NativeBookmarkEditionStage>(scratch);
    ASSERT_TRUE(stage);
    const auto expected = mode == 0   ? TintaJournalResult::Ok
                          : mode == 1 ? TintaJournalResult::Conflict
                                      : TintaJournalResult::Invalid;
    ASSERT_EQ(stage->prepare(event.resource, 4), expected);
    std::span<const uint8_t> output;
    if (mode == 0) {
      EXPECT_EQ(stage->size(), 2u);
      ASSERT_EQ(stage->next(output), BookmarkCursorResult::Found);
      BookmarkBodyView decoded;
      ASSERT_TRUE(decodeBookmarkBody(output, decoded));
      EXPECT_EQ(decoded.identity[0], 3);
      EXPECT_FALSE(decoded.deleted);
      ASSERT_EQ(stage->next(output), BookmarkCursorResult::Found);
      ASSERT_TRUE(decodeBookmarkBody(output, decoded));
      EXPECT_EQ(decoded.identity[0], 7);
      EXPECT_TRUE(decoded.deleted);
      ASSERT_EQ(stage->next(output), BookmarkCursorResult::End);
    } else {
      EXPECT_EQ(stage->size(), 0u);
      EXPECT_EQ(stage->next(output), BookmarkCursorResult::Error);
      EXPECT_TRUE(output.empty());
      EXPECT_FALSE(state.files.count(HalBookmarkBodyStage::PATH));
    }
    EXPECT_EQ(state.files.at("/bookmarks.json"), original);
    EXPECT_FALSE(state.files.count(HalBookmarkIdentityStage::PATH));
    EXPECT_TRUE(stage->cleanup());
    EXPECT_FALSE(state.files.count(HalBookmarkBodyStage::PATH));
  }
}

TEST(NativeReaderPreferenceMetadataTest, EditionStagePreservesForeignBodiesAndRefusesMissingAuthority) {
  for (const bool foreign : {false, true}) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    Digest edition{};
    edition.fill(7);
    if (foreign) {
      BookmarkBodyView bookmark;
      bookmark.identity[0] = 3;
      SaveIdentities identities;
      auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
      ASSERT_TRUE(save);
      ASSERT_EQ(save->persist(bookmark, edition, identities), TintaJournalResult::Ok);
      state.files[HalBookmarkBodyStage::PATH] = {1, 2, 3};
    }
    std::array<uint8_t, 128> scratch{};
    auto stage = makeUniqueNoThrow<NativeBookmarkEditionStage>(scratch);
    ASSERT_TRUE(stage);
    EXPECT_EQ(stage->prepare(edition, 4), foreign ? TintaJournalResult::IoError : TintaJournalResult::Unavailable);
    EXPECT_EQ(stage->size(), 0u);
    EXPECT_FALSE(state.files.count(HalBookmarkIdentityStage::PATH));
    stage.reset();
    if (foreign) EXPECT_EQ(state.files.at(HalBookmarkBodyStage::PATH), (std::vector<uint8_t>{1, 2, 3}));
  }
}

TEST(NativeReaderPreferenceMetadataTest, EditionBodyReadbackMustReachVerifiedEndBeforePublication) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  Digest edition{};
  edition.fill(7);
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 3;
  bookmark.anchor = {2, 42};
  SaveIdentities identities;
  auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(bookmark, edition, identities), TintaJournalResult::Ok);
  save.reset();
  std::array<uint8_t, 128> scratch{};
  auto stage = makeUniqueNoThrow<NativeBookmarkEditionStage>(scratch);
  ASSERT_TRUE(stage);
  ASSERT_EQ(stage->prepare(edition, 4), TintaJournalResult::Ok);
  state.files.at(HalBookmarkBodyStage::PATH)[22] ^= 1;
  std::span<const uint8_t> output;
  ASSERT_EQ(stage->next(output), BookmarkCursorResult::Found);
  EXPECT_EQ(stage->next(output), BookmarkCursorResult::Error);
  EXPECT_EQ(stage->size(), 0u);
  EXPECT_TRUE(stage->cleanup());
  EXPECT_FALSE(state.files.count(HalBookmarkBodyStage::PATH));
}

TEST(NativeReaderPreferenceMetadataTest, CompleteJsonPreparationChecksEveryStageAndLeavesCanonicalUntouched) {
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    state.files["/bookmarks.json"] = {'o', 'l', 'd'};
    const auto original = state.files.at("/bookmarks.json");
    Digest edition{};
    edition.fill(7);
    BookmarkBodyView bookmark;
    bookmark.identity[0] = 3;
    bookmark.anchor = {2, 42};
    SaveIdentities identities;
    auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
    ASSERT_TRUE(save);
    ASSERT_EQ(save->persist(bookmark, edition, identities), TintaJournalResult::Ok);
    save.reset();
    if (mode == 1) state.files[HalBookmarkJsonStage::PATH] = {1, 2, 3};
    if (mode == 2) state.failSyncPath = HalBookmarkJsonStage::PATH;
    if (mode == 3) state.corruptWritePath = HalBookmarkJsonStage::PATH;
    std::array<uint8_t, 128> scratch{};
    auto prepared = makeUniqueNoThrow<NativeBookmarkJsonPreparation>(scratch);
    ASSERT_TRUE(prepared);
    EXPECT_EQ(prepared->prepare(edition, 4), mode == 0 ? TintaJournalResult::Ok : TintaJournalResult::IoError);
    EXPECT_EQ(prepared->isPrepared(), mode == 0);
    EXPECT_EQ(state.files.at("/bookmarks.json"), original);
    EXPECT_FALSE(state.files.count(HalBookmarkIdentityStage::PATH));
    EXPECT_FALSE(state.files.count(HalBookmarkBodyStage::PATH));
    if (mode == 0) {
      const auto& bytes = state.files.at(HalBookmarkJsonStage::PATH);
      const std::string json(bytes.begin(), bytes.end());
      EXPECT_EQ(json,
                R"({"bookmarks":[{"id":"03000000000000000000000000000000","si":2,"vo":42,"name":"","summary":""}]})");
      EXPECT_EQ(prepared->bytesWritten(), bytes.size());
      EXPECT_EQ(prepared->contentEdition(), edition);
      HalJournalCausalAuditSession audit;
      Digest frontier{};
      ASSERT_TRUE(audit.run(&frontier));
      EXPECT_EQ(prepared->authorityFrontier(), frontier);
      EXPECT_EQ(prepared->recordCount(), audit.recordCount());
      EXPECT_EQ(prepared->recordSize(), audit.recordSize());
    } else if (mode == 1) {
      EXPECT_EQ(state.files.at(HalBookmarkJsonStage::PATH), (std::vector<uint8_t>{1, 2, 3}));
    } else {
      EXPECT_FALSE(state.files.count(HalBookmarkJsonStage::PATH));
    }
    state.failSyncPath.clear();
    state.corruptWritePath.clear();
    EXPECT_TRUE(prepared->cleanup());
  }
}

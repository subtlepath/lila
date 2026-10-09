#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <fstream>
#include <iterator>

#include "CompanionBookmarkChoicePage.h"
#include "CompanionBookmarkEditSession.h"
#include "CompanionBookmarkEditionStage.h"
#include "CompanionBookmarkIdentityEnumeration.h"
#include "CompanionBookmarkJsonPreparation.h"
#include "CompanionBookmarkLegacyImport.h"
#include "CompanionBookmarkPublicationSession.h"
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
  save.reset();
  save = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(save);
  EXPECT_EQ(save->persist(baseline, baseline, identities, 2), TintaJournalResult::Invalid);
  EXPECT_EQ(identities.calls, 0U);
  EXPECT_EQ(state.files, before);
  save.reset();
  save = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(save);
  EXPECT_EQ(save->persist(candidate, candidate, identities, 1), TintaJournalResult::Invalid);
  EXPECT_EQ(identities.calls, 0U);
  EXPECT_EQ(state.files, before);
}

TEST(NativeReaderPreferenceMetadataTest, ExplicitContentRefreshJournalsNewHashForUnchangedFontSelection) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  state.directories["/"] = {{".fonts", true}, {".crosspoint", true}};
  state.directories["/.fonts"] = {{"Test", true}};
  state.directories["/.fonts/Test"] = {{"Test_14.cpfont", false}};
  std::ifstream input(std::string(COMPANION_FIXTURE_DIR) + "/BitmapFont-v4.fixture", std::ios::binary);
  auto& font = state.files["/.fonts/Test/Test_14.cpfont"];
  font = {std::istreambuf_iterator<char>(input), {}};
  ASSERT_FALSE(font.empty());
  SdCardFontRegistry registry;
  ASSERT_TRUE(registry.discover());
  std::array<uint8_t, 1024> scratch{};
  ReaderPreferenceValues selected;
  selected.fontPointSize = 14;
  std::copy_n("Test", 5, selected.sdFontFamilyName.begin());
  SaveIdentities identities;
  auto save = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(selected, selected, identities, 1), TintaJournalResult::Ok);
  save.reset();
  const auto originalJournal = state.files.at(TINTA_JOURNAL_EVENTS);
  font.back() ^= 1;
  Digest expected{};
  ASSERT_EQ(EVP_Digest(font.data(), font.size(), expected.data(), nullptr, EVP_sha256(), nullptr), 1);
  save = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(save);
  const auto calls = identities.calls;
  ASSERT_EQ(save->persist(selected, selected, identities), TintaJournalResult::Ok);
  EXPECT_EQ(identities.calls, calls);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), originalJournal);
  save.reset();
  save = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(selected, selected, identities, 1), TintaJournalResult::Ok);
  EXPECT_FALSE(save->requiresRecovery());
  save.reset();
  HalTintaJournalStorage storage;
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  ASSERT_GT(journal.count(), 1U);
  ASSERT_EQ(journal.read(journal.count() - 1), TintaJournalResult::Ok);
  ASSERT_GE(journal.body().size(), 38U);
  EXPECT_EQ(journal.body()[2], 1);
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), journal.body().begin() + 5));
  ASSERT_TRUE(storage.close());
  const auto refreshedJournal = state.files.at(TINTA_JOURNAL_EVENTS);
  const auto refreshedCalls = identities.calls;
  save = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(selected, selected, identities, 1), TintaJournalResult::Ok);
  EXPECT_EQ(identities.calls, refreshedCalls);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), refreshedJournal);
  save.reset();
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  SyncEvent concurrent;
  concurrent.identity.epoch = concurrent.identity.sequence = 1;
  concurrent.storageGeneration.fill(2);
  concurrent.kind = EventKind::Preference;
  concurrent.resource = PREFERENCE_SCOPE;
  std::array<uint8_t, 8> margin{1, static_cast<uint8_t>(EventKind::Preference), 8, 1, 20, 0, 0, 0};
  for (uint8_t origin = 70; origin <= 71; ++origin) {
    concurrent.identity.origin.fill(origin);
    margin[4] = origin == 70 ? 20 : 30;
    ASSERT_TRUE(storage.digest(margin, concurrent.bodyHash));
    ASSERT_EQ(journal.append(concurrent, margin), TintaJournalResult::Ok);
  }
  ASSERT_TRUE(storage.close());
  const auto conflictingJournal = state.files.at(TINTA_JOURNAL_EVENTS);
  save = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(selected, selected, identities, 1), TintaJournalResult::Ok);
  EXPECT_EQ(identities.calls, refreshedCalls);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), conflictingJournal);
  save.reset();
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(journal.read(1), TintaJournalResult::Ok);
  std::vector<uint8_t> conflictingFont(journal.body().begin(), journal.body().end());
  ASSERT_EQ(conflictingFont[2], 1);
  conflictingFont[5] ^= 1;
  concurrent.identity.origin.fill(72);
  ASSERT_TRUE(storage.digest(conflictingFont, concurrent.bodyHash));
  ASSERT_EQ(journal.append(concurrent, conflictingFont), TintaJournalResult::Ok);
  ASSERT_TRUE(storage.close());
  auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
  auto resolution = makeUniqueNoThrow<PortablePreferenceResolution>();
  ASSERT_TRUE(audit);
  ASSERT_TRUE(resolution);
  ASSERT_TRUE(audit->run());
  ASSERT_EQ(audit->resolvePortablePreferences(*resolution), TintaJournalResult::Conflict);
  EXPECT_TRUE(resolution->readerBody(1).empty());
  EXPECT_EQ(resolution->conflictMask(), 0x81U);
  audit.reset();
  save = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(selected, selected, identities, 1), TintaJournalResult::Ok);
  EXPECT_GT(identities.calls, refreshedCalls);
  save.reset();
  audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
  ASSERT_TRUE(audit);
  ASSERT_TRUE(audit->run());
  ASSERT_EQ(audit->resolvePortablePreferences(*resolution), TintaJournalResult::Conflict);
  EXPECT_EQ(resolution->conflictMask(), 0x80U);
  const auto resolvedFont = resolution->readerBody(1);
  ASSERT_GE(resolvedFont.size(), 38U);
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), resolvedFont.begin() + 5));
}

TEST(NativeReaderPreferenceMetadataTest, DictionaryRefreshRequiresCompleteBundleAndDeduplicatesVerifiedBytes) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
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
  auto& definitions = state.files["/dictionaries/es/stem.dict"];
  definitions = {'o', 'n', 'e', 't', 'w', 'o'};
  const std::string info =
      "StarDict's dict ifo file\nversion=3.0.0\nbookname=Canonical "
      "dictionary\nwordcount=2\nidxfilesize=24\nsynwordcount=1\n";
  state.files["/dictionaries/es/stem.ifo"] = {info.begin(), info.end()};
  SdCardFontRegistry registry;
  std::array<uint8_t, 1024> scratch{};
  ReaderPreferenceValues selected;
  std::copy_n("es", 3, selected.dictionaryName.begin());
  SaveIdentities identities;
  const auto refresh = [&] {
    auto save = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(registry, scratch);
    EXPECT_TRUE(save);
    return save ? save->persist(selected, selected, identities, 0x400) : TintaJournalResult::IoError;
  };
  ASSERT_EQ(refresh(), TintaJournalResult::Ok);
  const auto originalJournal = state.files.at(TINTA_JOURNAL_EVENTS);
  const auto originalCalls = identities.calls;
  definitions.resize(2);
  EXPECT_EQ(refresh(), TintaJournalResult::Invalid);
  EXPECT_EQ(identities.calls, originalCalls);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), originalJournal);
  definitions = {'u', 'n', 'o', 't', 'w', 'o'};
  auto canonical = fixture("DictionaryBundle-plain.fixture");
  const std::array<uint8_t, 6> originalDefinitions{'o', 'n', 'e', 't', 'w', 'o'};
  const auto at =
      std::search(canonical.begin(), canonical.end(), originalDefinitions.begin(), originalDefinitions.end());
  ASSERT_NE(at, canonical.end());
  std::copy(definitions.begin(), definitions.end(), at);
  // Independent ZIP fixture: update data-descriptor and central-directory CRCs.
  const std::array<uint8_t, 4> originalCrc{0x45, 0x81, 0x11, 0x8c};
  const std::array<uint8_t, 4> changedCrc{0xdc, 0x40, 0xe1, 0x41};
  unsigned replaced = 0;
  auto cursor = canonical.begin();
  while ((cursor = std::search(cursor, canonical.end(), originalCrc.begin(), originalCrc.end())) != canonical.end()) {
    std::copy(changedCrc.begin(), changedCrc.end(), cursor);
    cursor += changedCrc.size();
    ++replaced;
  }
  ASSERT_EQ(replaced, 2U);
  Digest expected{};
  ASSERT_EQ(EVP_Digest(canonical.data(), canonical.size(), expected.data(), nullptr, EVP_sha256(), nullptr), 1);
  ASSERT_EQ(refresh(), TintaJournalResult::Ok);
  EXPECT_GT(identities.calls, originalCalls);
  HalTintaJournalStorage storage;
  TintaJournal journal(storage, scratch);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(journal.read(journal.count() - 1), TintaJournalResult::Ok);
  PreferenceBodyView body;
  ASSERT_TRUE(decodePreferenceBody(journal.body(), body));
  EXPECT_EQ(body.key, 11);
  ASSERT_EQ(body.contentHash.size(), expected.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), body.contentHash.begin()));
  ASSERT_TRUE(storage.close());
  const auto refreshedJournal = state.files.at(TINTA_JOURNAL_EVENTS);
  const auto refreshedCalls = identities.calls;
  EXPECT_EQ(refresh(), TintaJournalResult::Ok);
  EXPECT_EQ(identities.calls, refreshedCalls);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), refreshedJournal);
  HalDictionaryCacheStorage cache;
  DictionaryCachePublication publication(cache, scratch);
  DictionaryArchiveBinding binding;
  binding.members.kind = binding.original.kind = ContentKind::Dictionary;
  binding.members.formatVersion = binding.original.formatVersion = 1;
  binding.members.length = canonical.size();
  binding.members.contentHash = expected;
  state.files[DICTIONARY_CACHE_CANDIDATE] = canonical;
  ASSERT_EQ(publication.publish(binding.members), DictionaryCacheResult::Ok);
  auto originalArchive = canonical;
  originalArchive[originalArchive.size() - 2] = 3;
  originalArchive.insert(originalArchive.end(), {'z', 'i', 'p'});
  binding.original.length = originalArchive.size();
  ASSERT_EQ(EVP_Digest(originalArchive.data(), originalArchive.size(), binding.original.contentHash.data(), nullptr,
                       EVP_sha256(), nullptr),
            1);
  state.files[DICTIONARY_CACHE_CANDIDATE] = originalArchive;
  ASSERT_EQ(publication.publish(binding.original), DictionaryCacheResult::Ok);
  HalDictionaryBindings bindings(cache, scratch);
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", binding));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", binding));
  ASSERT_EQ(refresh(), TintaJournalResult::Ok);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  ASSERT_EQ(journal.read(journal.count() - 1), TintaJournalResult::Ok);
  ASSERT_TRUE(decodePreferenceBody(journal.body(), body));
  ASSERT_EQ(body.contentHash.size(), binding.original.contentHash.size());
  EXPECT_TRUE(
      std::equal(binding.original.contentHash.begin(), binding.original.contentHash.end(), body.contentHash.begin()));
  ASSERT_TRUE(storage.close());
  const auto boundFiles = state.files;
  const auto boundCalls = identities.calls;
  definitions[0] = 'd';
  const auto editedFiles = state.files;
  EXPECT_EQ(refresh(), TintaJournalResult::Invalid);
  EXPECT_EQ(identities.calls, boundCalls);
  EXPECT_EQ(state.files, editedFiles);
  definitions[0] = 'u';
  EXPECT_EQ(refresh(), TintaJournalResult::Ok);
  EXPECT_EQ(identities.calls, boundCalls);
  // Authority, bindings, retained archives and members remain unchanged; audit marks are disposable.
  for (const auto& [path, bytes] : boundFiles) {
    if (path.starts_with("/dictionaries/") || path.starts_with("/.crosspoint/companion/dictionary-") ||
        path.starts_with("/.crosspoint/companion/tinta-events/"))
      EXPECT_EQ(state.files.at(path), bytes) << path;
  }
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
    bookmark.deleted = origin == 6;
    event.kind = bookmark.deleted ? EventKind::BookmarkDelete : EventKind::BookmarkPut;
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
  auto choices = makeUniqueNoThrow<NativeBookmarkChoicePage>();
  ASSERT_TRUE(choices);
  ASSERT_EQ(choices->load(edition, bookmark.identity, 4), TintaJournalResult::Ok);
  ASSERT_EQ(choices->count(), 4u);
  EXPECT_EQ(choices->totalCount(), 6u);
  EXPECT_TRUE(choices->hasNext());
  BookmarkBodyView selected;
  ASSERT_TRUE(choices->choice(0, selected));
  EXPECT_TRUE(selected.deleted);
  EXPECT_EQ(choices->source(0)->origin[0], 6u);
  EXPECT_EQ(identities.calls, 0u);
  auto next = makeUniqueNoThrow<NativeBookmarkChoicePage>();
  ASSERT_TRUE(next);
  ASSERT_EQ(next->load(edition, bookmark.identity, 4, 4), TintaJournalResult::Ok);
  EXPECT_EQ(next->count(), 2u);
  EXPECT_EQ(next->totalCount(), 6u);
  EXPECT_FALSE(next->hasNext());
  ASSERT_TRUE(next->choice(0, selected));
  EXPECT_EQ(selected.anchor.visibleTextOffset, 10u);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), before);
  ASSERT_EQ(choices->resolve(0, identities), TintaJournalResult::Ok);
  EXPECT_EQ(choices->count(), 0u);
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

TEST(NativeReaderPreferenceMetadataTest, BookmarkChoiceRejectsAnUnseenConcurrentEditBeforeReservingIdentity) {
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
  event.resource = edition;
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 3;
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  const auto append = [&](uint8_t origin, bool deleted) {
    event.identity.origin.fill(origin);
    bookmark.deleted = deleted;
    event.kind = deleted ? EventKind::BookmarkDelete : EventKind::BookmarkPut;
    bookmark.anchor = {2, static_cast<uint32_t>(origin * 5)};
    const auto size = encodeBookmarkBody(bookmark, body);
    EXPECT_TRUE(storage.digest(std::span(body).first(size), event.bodyHash));
    EXPECT_EQ(journal.append(event, std::span(body).first(size)), TintaJournalResult::Ok);
  };
  append(1, false);
  append(2, true);
  ASSERT_TRUE(storage.close());
  auto choices = makeUniqueNoThrow<NativeBookmarkChoicePage>();
  ASSERT_TRUE(choices);
  ASSERT_EQ(choices->load(edition, bookmark.identity, 4), TintaJournalResult::Ok);
  ASSERT_EQ(choices->count(), 2u);
  ASSERT_EQ(journal.open(), TintaJournalResult::Ok);
  append(3, false);
  ASSERT_TRUE(storage.close());
  const auto before = state.files.at(TINTA_JOURNAL_EVENTS);
  EXPECT_EQ(choices->resolve(0, identities), TintaJournalResult::Conflict);
  EXPECT_EQ(identities.calls, 0u);
  EXPECT_FALSE(choices->requiresRecovery());
  EXPECT_EQ(choices->count(), 0u);
  EXPECT_EQ(choices->resolve(0, identities), TintaJournalResult::Invalid);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), before);
  ASSERT_EQ(choices->load(edition, bookmark.identity, 4), TintaJournalResult::Ok);
  ASSERT_EQ(choices->count(), 3u);
  BookmarkBodyView selected;
  ASSERT_TRUE(choices->choice(1, selected));
  ASSERT_TRUE(selected.deleted);
  ASSERT_EQ(choices->resolve(1, identities), TintaJournalResult::Ok);
  HalJournalCausalAuditSession audit;
  ASSERT_TRUE(audit.run());
  size_t length = 0;
  ASSERT_EQ(audit.resolveBookmark(edition, bookmark.identity, body, length), TintaJournalResult::Ok);
  ASSERT_TRUE(decodeBookmarkBody(std::span(body).first(length), selected));
  EXPECT_TRUE(selected.deleted);
  const auto resolved = state.files.at(TINTA_JOURNAL_EVENTS);
  const auto calls = identities.calls;
  ASSERT_EQ(choices->load(edition, bookmark.identity, 4), TintaJournalResult::Ok);
  EXPECT_EQ(choices->count(), 1u);
  ASSERT_EQ(choices->resolve(0, identities), TintaJournalResult::Ok);
  EXPECT_EQ(identities.calls, calls);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), resolved);
}

TEST(NativeReaderPreferenceMetadataTest, BookmarkChoiceClearsReadinessOnFailedReloadAndRejectsBadSelections) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  SaveIdentities identities;
  Digest edition{};
  edition.fill(7);
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 3;
  bookmark.anchor = {3, 42};
  auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
  ASSERT_TRUE(save);
  ASSERT_EQ(save->persist(bookmark, edition, identities), TintaJournalResult::Ok);
  auto choices = makeUniqueNoThrow<NativeBookmarkChoicePage>();
  ASSERT_TRUE(choices);
  ASSERT_EQ(choices->load(edition, bookmark.identity, 4), TintaJournalResult::Ok);
  EXPECT_EQ(choices->resolve(4, identities), TintaJournalResult::Invalid);
  EXPECT_EQ(choices->count(), 1u);
  EXPECT_EQ(choices->load(edition, bookmark.identity, 4, 1), TintaJournalResult::Unavailable);
  EXPECT_EQ(choices->count(), 0u);
  EXPECT_EQ(choices->resolve(0, identities), TintaJournalResult::Invalid);
  const auto before = state.files.at(TINTA_JOURNAL_EVENTS);
  const auto calls = identities.calls;
  state.failClose = true;
  EXPECT_EQ(choices->load(edition, bookmark.identity, 4), TintaJournalResult::IoError);
  EXPECT_EQ(choices->count(), 0u);
  EXPECT_EQ(choices->resolve(0, identities), TintaJournalResult::Invalid);
  EXPECT_EQ(identities.calls, calls);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), before);
  state.failClose = false;
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
    EXPECT_EQ(stage->conflictIdentity()[0], mode == 1 ? 7 : 0);
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

TEST(NativeReaderPreferenceMetadataTest, EmptyEditionPreparationRequiresOptInAndCreatesNoJournalEvents) {
  for (const bool allowEmpty : {false, true}) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    Digest edition{};
    edition.fill(7);
    std::array<uint8_t, 128> scratch{};
    auto prepared = makeUniqueNoThrow<NativeBookmarkJsonPreparation>(scratch);
    ASSERT_TRUE(prepared);
    EXPECT_EQ(prepared->prepare(edition, 4, allowEmpty),
              allowEmpty ? TintaJournalResult::Ok : TintaJournalResult::Unavailable);
    EXPECT_EQ(prepared->isPrepared(), allowEmpty);
    EXPECT_EQ(prepared->recordCount(), 0u);
    EXPECT_FALSE(state.files.count(HalBookmarkIdentityStage::PATH));
    EXPECT_FALSE(state.files.count(HalBookmarkBodyStage::PATH));
    if (allowEmpty) {
      const auto& bytes = state.files.at(HalBookmarkJsonStage::PATH);
      EXPECT_EQ(std::string(bytes.begin(), bytes.end()), R"({"bookmarks":[]})");
      HalJournalCausalAuditSession audit;
      Digest frontier{};
      ASSERT_TRUE(audit.run(&frontier));
      EXPECT_EQ(prepared->authorityFrontier(), frontier);
      EXPECT_EQ(audit.recordCount(), 0u);
    }
    EXPECT_TRUE(prepared->cleanup());
    EXPECT_FALSE(state.files.count(HalBookmarkJsonStage::PATH));
  }
}

TEST(NativeReaderPreferenceMetadataTest, EmptyJournalPublicationRecoversAndRepeatedPublicationChangesNoFiles) {
  for (const bool editionPath : {false, true}) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
    ASSERT_TRUE(Storage.ensureDirectoryExists(HalBookmarkPublicationPaths::ACTIVE_PARENT));
    Digest edition{};
    edition.fill(7);
    constexpr const char* canonical =
        "/.crosspoint/bookmarks/editions/0707070707070707070707070707070707070707070707070707070707070707.json";
    const char* active = editionPath ? canonical : "/.crosspoint/bookmarks/book.json";
    if (editionPath) ASSERT_TRUE(Storage.ensureDirectoryExists(BOOKMARK_EDITION_CACHE_PARENT));
    state.files[active] = {'o', 'l', 'd'};
    Identity transaction{}, generation{};
    transaction.fill(1);
    generation.fill(2);
    std::array<uint8_t, 128> scratch{};
    const auto validator = +[](void*, const BookmarkPublicationClaim&) { return true; };
    const auto preparationValidator = +[](void*, const BookmarkPreparationClaim&) { return true; };
    state.failRenameAfterSource = HalBookmarkPublicationPaths::INTENT_NEXT;
    auto session =
        makeUniqueNoThrow<NativeBookmarkPublicationSession>(scratch, validator, nullptr, preparationValidator);
    ASSERT_TRUE(session);
    EXPECT_EQ(session->publish(active, edition, 4, transaction, generation, true), TintaJournalResult::IoError);
    EXPECT_TRUE(session->requiresRecovery());
    const auto pendingClaim = session->publicationClaim();
    session.reset();
    EXPECT_EQ(state.files.at(active), (std::vector<uint8_t>{'o', 'l', 'd'}));
    ASSERT_TRUE(state.files.contains(HalBookmarkPublicationPaths::INTENT));
    ASSERT_TRUE(state.files.contains(HalBookmarkJsonStage::PATH));
    state.failRenameAfterSource.clear();
    constexpr const char* wrong = "/.crosspoint/bookmarks/other.json";
    state.files[wrong] = {'o', 'l', 'd'};
    const auto beforeWrongRecovery = state.files;
    session = makeUniqueNoThrow<NativeBookmarkPublicationSession>(scratch, validator, nullptr, preparationValidator);
    ASSERT_TRUE(session);
    EXPECT_EQ(session->recoverPending(wrong), TintaJournalResult::Invalid);
    session.reset();
    EXPECT_EQ(state.files, beforeWrongRecovery);
    session = makeUniqueNoThrow<NativeBookmarkPublicationSession>(scratch, validator, nullptr, preparationValidator);
    ASSERT_TRUE(session);
    EXPECT_EQ(session->recover(wrong, pendingClaim), TintaJournalResult::Invalid);
    session.reset();
    EXPECT_EQ(state.files, beforeWrongRecovery);
    const auto preparationBytes = state.files.at(HalBookmarkPreparationStorage::PATH);
    for (unsigned mode = 0; mode < 3; ++mode) {
      if (mode == 0) state.files[HalBookmarkPreparationStorage::PATH][0] ^= 1;
      if (mode == 1) state.files.erase(HalBookmarkPreparationStorage::PATH);
      if (mode == 2) state.failClosePath = HalBookmarkPreparationStorage::PATH;
      const auto protectedFiles = state.files;
      session = makeUniqueNoThrow<NativeBookmarkPublicationSession>(scratch, validator, nullptr, preparationValidator);
      ASSERT_TRUE(session);
      EXPECT_EQ(session->recoverPending(active), mode == 2 ? TintaJournalResult::IoError : TintaJournalResult::Corrupt);
      session.reset();
      EXPECT_EQ(state.files, protectedFiles);
      state.failClosePath.clear();
      state.files[HalBookmarkPreparationStorage::PATH] = preparationBytes;
    }
    session = makeUniqueNoThrow<NativeBookmarkPublicationSession>(scratch, validator, nullptr, preparationValidator);
    ASSERT_TRUE(session);
    ASSERT_EQ(session->recoverPending(active), TintaJournalResult::Ok);
    session.reset();
    const auto& bytes = state.files.at(active);
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()), R"({"bookmarks":[]})");
    const auto files = state.files;
    transaction.fill(9);
    session = makeUniqueNoThrow<NativeBookmarkPublicationSession>(scratch, validator, nullptr, preparationValidator);
    ASSERT_TRUE(session);
    EXPECT_EQ(session->publish(active, edition, 4, transaction, generation, true), TintaJournalResult::Ok);
    session.reset();
    EXPECT_EQ(state.files, files);
    HalJournalCausalAuditSession audit;
    ASSERT_TRUE(audit.run());
    EXPECT_EQ(audit.recordCount(), 0u);
  }
}

TEST(NativeReaderPreferenceMetadataTest, CompleteBookmarkPublicationPreservesCandidateOnUncertainIntentAndRecovers) {
  for (unsigned mode = 0; mode < 9; ++mode) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    ASSERT_TRUE(Storage.ensureDirectoryExists(HalBookmarkPublicationPaths::ACTIVE_PARENT));
    constexpr const char* active = "/.crosspoint/bookmarks/book.json";
    state.files[active] = {'o', 'l', 'd'};
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
    Identity transaction{}, generation{};
    transaction.fill(1);
    generation.fill(2);
    bool validContext = mode != 3;
    const auto validator = +[](void* opaque, const BookmarkPublicationClaim&) { return *static_cast<bool*>(opaque); };
    auto session = makeUniqueNoThrow<NativeBookmarkPublicationSession>(
        scratch, validator, &validContext,
        +[](void* ctx, const BookmarkPreparationClaim&) { return *static_cast<bool*>(ctx); });
    ASSERT_TRUE(session);
    if (mode == 1) state.failRenameAfterSource = HalBookmarkPublicationPaths::INTENT_NEXT;
    if (mode == 8) state.failRenameAfterSource = HalBookmarkPublicationPaths::INTENT_NEXT;
    if (mode == 2) state.failSyncPath = HalBookmarkPublicationPaths::INTENT_NEXT;
    if (mode == 4) state.files[HalBookmarkPublicationPaths::INTENT] = {9};
    if (mode == 5) state.failSyncPath = HalBookmarkPreparationStorage::NEXT;
    if (mode == 6) state.failRenameAfterSource = HalBookmarkPreparationStorage::NEXT;
    if (mode == 7) {
      state.failWritePath = HalBookmarkIdentityStage::PATH;
      state.failMatchingWrite = 1;
      state.failRemove = true;
    }
    const auto expected = mode == 0   ? TintaJournalResult::Ok
                          : mode == 3 ? TintaJournalResult::Invalid
                          : mode == 4 ? TintaJournalResult::Conflict
                                      : TintaJournalResult::IoError;
    EXPECT_EQ(session->publish(active, edition, 4, transaction, generation), expected);
    EXPECT_FALSE(tinta_body_detail::nonzero(session->conflictIdentity()));
    EXPECT_EQ(session->requiresRecovery(), mode == 1 || mode == 2 || mode >= 5);
    session.reset();
    if (mode >= 3 && mode != 8) {
      EXPECT_EQ(state.files.at(active), (std::vector<uint8_t>{'o', 'l', 'd'}));
      EXPECT_FALSE(state.files.contains(HalBookmarkJsonStage::PATH));
      if (mode == 4) EXPECT_EQ(state.files.at(HalBookmarkPublicationPaths::INTENT), (std::vector<uint8_t>{9}));
      if (mode >= 5) {
        if (mode < 7) EXPECT_FALSE(state.files.contains(HalBookmarkIdentityStage::PATH));
        state.failSyncPath.clear();
        state.failRenameAfterSource.clear();
        state.failWritePath.clear();
        state.failRemove = false;
        auto orphan = makeUniqueNoThrow<NativeBookmarkPreparationSession>(
            +[](void*, const BookmarkPreparationClaim&) { return true; }, nullptr);
        ASSERT_EQ(orphan->recover(active, edition, generation), TintaJournalResult::Ok);
        EXPECT_FALSE(state.files.contains(HalBookmarkPreparationStorage::PATH));
        EXPECT_FALSE(state.files.contains(HalBookmarkIdentityStage::PATH));
      }
      continue;
    }
    if (mode) {
      EXPECT_TRUE(state.files.contains(HalBookmarkJsonStage::PATH));
      EXPECT_TRUE(state.files.contains(HalBookmarkPreparationStorage::PATH));
      if (mode == 8) state.files.erase(HalBookmarkPreparationStorage::PATH);
      EXPECT_EQ(state.files.at(active), (std::vector<uint8_t>{'o', 'l', 'd'}));
      state.failRenameAfterSource.clear();
      state.failSyncPath.clear();
      session = makeUniqueNoThrow<NativeBookmarkPublicationSession>(
          scratch, validator, &validContext,
          +[](void* ctx, const BookmarkPreparationClaim&) { return *static_cast<bool*>(ctx); });
      ASSERT_EQ(session->recoverPending(active), TintaJournalResult::Ok);
      EXPECT_FALSE(session->requiresRecovery());
    }
    const auto& bytes = state.files.at(active);
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()),
              R"({"bookmarks":[{"id":"03000000000000000000000000000000","si":2,"vo":42,"name":"","summary":""}]})");
    EXPECT_FALSE(state.files.contains(HalBookmarkJsonStage::PATH));
    EXPECT_FALSE(state.files.contains(HalBookmarkPreparationStorage::PATH));
  }
}

TEST(NativeReaderPreferenceMetadataTest, BookmarkRecoveryDoesNotPromoteWrongContextOrCorruptOwnership) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  ASSERT_TRUE(Storage.ensureDirectoryExists(TRANSFER_DIRECTORY));
  std::array<uint8_t, 128> scratch{};
  const auto validator = +[](void*, const BookmarkPublicationClaim&) { return false; };
  constexpr const char* active = "/.crosspoint/bookmarks/book.json";
  auto session = makeUniqueNoThrow<NativeBookmarkPublicationSession>(
      scratch, validator, nullptr, +[](void*, const BookmarkPreparationClaim&) { return false; });
  EXPECT_EQ(session->recoverPending(active), TintaJournalResult::Unavailable);
  session.reset();
  state.files[HalBookmarkPublicationPaths::INTENT] = {9};
  const auto corrupt = state.files;
  session = makeUniqueNoThrow<NativeBookmarkPublicationSession>(
      scratch, validator, nullptr, +[](void*, const BookmarkPreparationClaim&) { return false; });
  EXPECT_EQ(session->recoverPending(active), TintaJournalResult::Corrupt);
  session.reset();
  EXPECT_EQ(state.files, corrupt);
  state.files.erase(HalBookmarkPublicationPaths::INTENT);
  BookmarkPublicationClaim claim;
  claim.transaction.fill(1);
  claim.storageGeneration.fill(2);
  claim.edition.fill(3);
  claim.frontier.fill(4);
  claim.candidateHash.fill(5);
  claim.candidateLength = 50;
  claim.recordCount = 1;
  claim.recordSize = 512;
  std::array<uint8_t, BOOKMARK_PUBLICATION_RECORD_SIZE> bytes{};
  ASSERT_TRUE(encodeBookmarkPublicationRecord(claim, bytes));
  state.files[HalBookmarkPublicationPaths::INTENT_NEXT] = std::vector<uint8_t>(bytes.begin(), bytes.end());
  const auto before = state.files;
  session = makeUniqueNoThrow<NativeBookmarkPublicationSession>(
      scratch, validator, nullptr, +[](void*, const BookmarkPreparationClaim&) { return false; });
  EXPECT_EQ(session->recoverPending(active), TintaJournalResult::Invalid);
  EXPECT_TRUE(session->requiresRecovery());
  EXPECT_EQ(state.files, before);
}

TEST(NativeReaderPreferenceMetadataTest, LegacyBookmarkImportIsRepeatableAndDoesNotResurrectJournalDeletion) {
  inventory_hal_test::state = {};
  inventory_hal_test::state.enumerateFileMap = true;
  Digest edition{}, originalHash{};
  edition.fill(7);
  originalHash.fill(8);
  std::array<BookmarkEntry, 2> entries{};
  for (auto& entry : entries) {
    entry.hasVisibleTextOffset = true;
    entry.computedSpineIndex = 2;
    entry.visibleTextOffset = 42;
  }
  entries[0].name = "First";
  entries[1].name = "Second";
  auto importer = makeUniqueNoThrow<NativeBookmarkLegacyImport>();
  ASSERT_TRUE(importer);
  Identity first{}, second{}, repeat{};
  ASSERT_TRUE(importer->identityFor(entries[0], 0, edition, originalHash, first));
  ASSERT_TRUE(importer->identityFor(entries[1], 1, edition, originalHash, second));
  EXPECT_NE(first, second);
  ASSERT_TRUE(importer->identityFor(entries[0], 0, edition, originalHash, repeat));
  EXPECT_EQ(first, repeat);
  Identity pinned{};
  ASSERT_TRUE(BookmarkIdentity::decode("3fd9a454c9e4eb76c4195eee2e102908", pinned));
  EXPECT_EQ(first, pinned);
  SaveIdentities identities;
  ASSERT_EQ(importer->import(entries, edition, originalHash, 4, identities), TintaJournalResult::Ok);
  EXPECT_FALSE(importer->requiresRecovery());
  importer.reset();
  const auto before = inventory_hal_test::state.files.at(TINTA_JOURNAL_EVENTS);
  importer = makeUniqueNoThrow<NativeBookmarkLegacyImport>();
  ASSERT_EQ(importer->import(entries, edition, originalHash, 4, identities), TintaJournalResult::Ok);
  EXPECT_EQ(inventory_hal_test::state.files.at(TINTA_JOURNAL_EVENTS), before);
  importer.reset();
  BookmarkBodyView deletion;
  deletion.identity = first;
  deletion.deleted = true;
  auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
  ASSERT_EQ(save->persist(deletion, edition, identities), TintaJournalResult::Ok);
  save.reset();
  const auto deleted = inventory_hal_test::state.files.at(TINTA_JOURNAL_EVENTS);
  importer = makeUniqueNoThrow<NativeBookmarkLegacyImport>();
  ASSERT_EQ(importer->import(entries, edition, originalHash, 4, identities), TintaJournalResult::Ok);
  EXPECT_EQ(inventory_hal_test::state.files.at(TINTA_JOURNAL_EVENTS), deleted);
}
TEST(NativeReaderPreferenceMetadataTest, LegacyBookmarkImportRejectsForeignEditionPutsAndDeletionsBeforeAnyAppend) {
  for (const bool deleted : {false, true}) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    Digest edition{}, foreign{}, originalHash{};
    edition.fill(7);
    foreign.fill(9);
    originalHash.fill(8);
    SaveIdentities identities;
    BookmarkBodyView bookmark;
    bookmark.identity[0] = 3;
    bookmark.deleted = deleted;
    auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
    ASSERT_TRUE(save);
    ASSERT_EQ(save->persist(bookmark, foreign, identities), TintaJournalResult::Ok);
    save.reset();
    const auto before = state.files.at(TINTA_JOURNAL_EVENTS);
    const auto calls = identities.calls;
    std::array<BookmarkEntry, 2> entries{};
    for (auto& entry : entries) {
      entry.hasVisibleTextOffset = true;
      entry.computedSpineIndex = 2;
      entry.visibleTextOffset = 42;
    }
    entries[0].name = "Missing current-edition bookmark";
    entries[1].identity = bookmark.identity;
    entries[1].name = "Foreign bookmark";
    auto importer = makeUniqueNoThrow<NativeBookmarkLegacyImport>();
    ASSERT_TRUE(importer);
    EXPECT_EQ(importer->import(entries, edition, originalHash, 4, identities), TintaJournalResult::Conflict);
    EXPECT_FALSE(tinta_body_detail::nonzero(importer->conflictIdentity()));
    EXPECT_FALSE(importer->requiresRecovery());
    EXPECT_EQ(identities.calls, calls);
    EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), before);
    EXPECT_EQ(entries[1].identity, bookmark.identity);
  }
}

TEST(NativeReaderPreferenceMetadataTest, LegacyBookmarkImportPreservesExistingCurrentEditionAuthorityWithSharedId) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  Digest edition{}, foreign{}, originalHash{};
  edition.fill(7);
  foreign.fill(9);
  originalHash.fill(8);
  SaveIdentities identities;
  BookmarkBodyView bookmark;
  bookmark.identity[0] = 3;
  for (const auto& resource : {foreign, edition}) {
    auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
    ASSERT_TRUE(save);
    ASSERT_EQ(save->persist(bookmark, resource, identities), TintaJournalResult::Ok);
  }
  const auto before = state.files.at(TINTA_JOURNAL_EVENTS);
  const auto calls = identities.calls;
  BookmarkEntry entry;
  entry.identity = bookmark.identity;
  entry.hasVisibleTextOffset = true;
  entry.name = "Stale legacy copy";
  entry.visibleTextOffset = 99;
  auto importer = makeUniqueNoThrow<NativeBookmarkLegacyImport>();
  ASSERT_TRUE(importer);
  EXPECT_EQ(importer->import(std::span(&entry, 1), edition, originalHash, 4, identities), TintaJournalResult::Ok);
  EXPECT_EQ(identities.calls, calls);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), before);
  EXPECT_FALSE(tinta_body_detail::nonzero(importer->conflictIdentity()));
}

TEST(NativeReaderPreferenceMetadataTest, LegacyBookmarkImportPreflightsLateInvalidEntriesWithoutJournalMutation) {
  for (unsigned mode = 0; mode < 6; ++mode) {
    inventory_hal_test::state = {};
    Digest edition{}, originalHash{};
    edition.fill(7);
    originalHash.fill(8);
    std::array<BookmarkEntry, 2> entries{};
    for (auto& entry : entries) {
      entry.hasVisibleTextOffset = true;
      entry.computedSpineIndex = 2;
    }
    if (mode == 0) entries[1].hasVisibleTextOffset = false;
    if (mode == 1) entries[1].computedSpineIndex = 99;
    if (mode == 2) {
      entries[0].identity.fill(3);
      entries[1].identity = entries[0].identity;
    }
    if (mode == 3) entries[1].name.assign(129, 'x');
    if (mode == 4) entries[1].summary.assign(513, 'x');
    if (mode == 5) entries[1].name = std::string("\xc0\x80", 2);
    auto importer = makeUniqueNoThrow<NativeBookmarkLegacyImport>();
    SaveIdentities identities;
    const auto before = inventory_hal_test::state.files;
    EXPECT_EQ(importer->import(entries, edition, originalHash, 4, identities), TintaJournalResult::Invalid);
    EXPECT_EQ(inventory_hal_test::state.files, before);
  }
}

TEST(NativeReaderPreferenceMetadataTest, LegacyBookmarkPartialImportRetriesOnlyMissingAuthority) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  Digest edition{}, originalHash{};
  edition.fill(7);
  originalHash.fill(8);
  std::array<BookmarkEntry, 2> entries{};
  for (auto& entry : entries) {
    entry.hasVisibleTextOffset = true;
    entry.computedSpineIndex = 2;
  }
  entries[0].name = "First";
  entries[1].name = "Second";
  SaveIdentities identities;
  auto importer = makeUniqueNoThrow<NativeBookmarkLegacyImport>();
  ASSERT_EQ(importer->import(std::span(entries).first(1), edition, originalHash, 4, identities),
            TintaJournalResult::Ok);
  importer.reset();
  const auto prefix = state.files.at(TINTA_JOURNAL_EVENTS);
  state.failWritePath = TINTA_JOURNAL_EVENTS;
  state.failMatchingWrite = 1;
  importer = makeUniqueNoThrow<NativeBookmarkLegacyImport>();
  EXPECT_EQ(importer->import(entries, edition, originalHash, 4, identities), TintaJournalResult::IoError);
  EXPECT_TRUE(importer->requiresRecovery());
  importer.reset();
  state.failWritePath.clear();
  state.failMatchingWrite = 0;
  importer = makeUniqueNoThrow<NativeBookmarkLegacyImport>();
  ASSERT_EQ(importer->import(entries, edition, originalHash, 4, identities), TintaJournalResult::Ok);
  const auto& events = state.files.at(TINTA_JOURNAL_EVENTS);
  EXPECT_GT(events.size(), prefix.size());
  EXPECT_TRUE(std::equal(prefix.begin(), prefix.end(), events.begin()));
  const auto complete = events;
  importer.reset();
  importer = makeUniqueNoThrow<NativeBookmarkLegacyImport>();
  ASSERT_EQ(importer->import(entries, edition, originalHash, 4, identities), TintaJournalResult::Ok);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), complete);
}

TEST(NativeReaderPreferenceMetadataTest, BookmarkEditPreflightsWithoutEntropyOrJournalMutation) {
  inventory_hal_test::state = {};
  inventory_hal_test::state.enumerateFileMap = true;
  SaveIdentities identities;
  Digest edition{};
  edition.fill(7);
  BookmarkEntry entry{};
  entry.name = "Name";
  entry.summary = "Summary";
  for (unsigned invalid = 0; invalid < 4; ++invalid) {
    entry.hasVisibleTextOffset = invalid != 0;
    entry.computedSpineIndex = invalid == 1 ? 9 : 0;
    entry.name = invalid == 2 ? std::string(129, 'a') : "Name";
    entry.summary = invalid == 3 ? std::string("bad\0text", 8) : "Summary";
    auto edit = makeUniqueNoThrow<NativeBookmarkEditSession>();
    ASSERT_TRUE(edit);
    EXPECT_EQ(edit->create(entry, edition, 4, identities), TintaJournalResult::Invalid);
    EXPECT_FALSE(BookmarkIdentity::valid(entry.identity));
    EXPECT_EQ(identities.calls, 0u);
    EXPECT_TRUE(inventory_hal_test::state.files.empty());
  }
}
TEST(NativeReaderPreferenceMetadataTest, BookmarkEditCreatesRenamesAndDeletesStableIdentityBeforeUiMutation) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  SaveIdentities identities;
  Digest edition{};
  edition.fill(7);
  BookmarkEntry entry{};
  entry.name = "Original";
  entry.summary = "Summary";
  entry.hasVisibleTextOffset = true;
  entry.computedSpineIndex = 2;
  entry.visibleTextOffset = 42;
  auto edit = makeUniqueNoThrow<NativeBookmarkEditSession>();
  ASSERT_EQ(edit->create(entry, edition, 4, identities), TintaJournalResult::Ok);
  ASSERT_TRUE(BookmarkIdentity::valid(entry.identity));
  const auto identity = entry.identity;
  edit = makeUniqueNoThrow<NativeBookmarkEditSession>();
  ASSERT_EQ(edit->rename(entry, "Renamed", edition, 4, identities), TintaJournalResult::Ok);
  EXPECT_EQ(entry.name, "Original");
  EXPECT_EQ(entry.identity, identity);
  auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
  ASSERT_TRUE(audit->run());
  std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> bytes{};
  size_t length = 0;
  ASSERT_EQ(audit->resolveBookmark(edition, identity, bytes, length), TintaJournalResult::Ok);
  BookmarkBodyView body;
  ASSERT_TRUE(decodeBookmarkBody(std::span(bytes).first(length), body));
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(body.name.data()), body.name.size()), "Renamed");
  EXPECT_EQ(body.anchor, (ReadingAnchor{2, 42}));
  audit.reset();
  edit = makeUniqueNoThrow<NativeBookmarkEditSession>();
  ASSERT_EQ(edit->erase(entry, edition, identities), TintaJournalResult::Ok);
  const auto events = state.files.at(TINTA_JOURNAL_EVENTS);
  const auto calls = identities.calls;
  edit = makeUniqueNoThrow<NativeBookmarkEditSession>();
  EXPECT_EQ(edit->erase(entry, edition, identities), TintaJournalResult::Ok);
  EXPECT_EQ(identities.calls, calls);
  EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), events);
  audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
  ASSERT_TRUE(audit->run());
  ASSERT_EQ(audit->resolveBookmark(edition, identity, bytes, length), TintaJournalResult::Ok);
  ASSERT_TRUE(decodeBookmarkBody(std::span(bytes).first(length), body));
  EXPECT_TRUE(body.deleted);
}
TEST(NativeReaderPreferenceMetadataTest, BookmarkCreationCollisionCannotReuseOrResurrectExistingIdentity) {
  for (const bool deleted : {false, true}) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    SaveIdentities identities;
    Digest edition{};
    edition.fill(7);
    BookmarkBodyView prior;
    prior.identity.fill(8);
    prior.deleted = deleted;
    auto save = makeUniqueNoThrow<NativeBookmarkSaveSession>();
    ASSERT_EQ(save->persist(prior, edition, identities), TintaJournalResult::Ok);
    save.reset();
    const auto before = state.files.at(TINTA_JOURNAL_EVENTS);
    identities.random = 8;
    BookmarkEntry entry{};
    entry.hasVisibleTextOffset = true;
    auto edit = makeUniqueNoThrow<NativeBookmarkEditSession>();
    EXPECT_EQ(edit->create(entry, edition, 1, identities), TintaJournalResult::Conflict);
    EXPECT_FALSE(BookmarkIdentity::valid(entry.identity));
    EXPECT_EQ(state.files.at(TINTA_JOURNAL_EVENTS), before);
  }
}
TEST(NativeReaderPreferenceMetadataTest, BookmarkCreationLeavesUiIdentityUnsetOnUncertainJournalFailure) {
  auto& state = inventory_hal_test::state;
  state = {};
  state.enumerateFileMap = true;
  state.failWrite = true;
  SaveIdentities identities;
  Digest edition{};
  edition.fill(7);
  BookmarkEntry entry{};
  entry.hasVisibleTextOffset = true;
  auto edit = makeUniqueNoThrow<NativeBookmarkEditSession>();
  EXPECT_EQ(edit->create(entry, edition, 1, identities), TintaJournalResult::IoError);
  EXPECT_TRUE(edit->requiresRecovery());
  EXPECT_FALSE(BookmarkIdentity::valid(entry.identity));
}

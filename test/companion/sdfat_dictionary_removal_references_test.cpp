#include <openssl/evp.h>

#include <cassert>
#include <fstream>
#include <iterator>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalDictionaryCacheStorage.h"
#include "lib/hal/HalDictionaryRemovalReferences.h"
#include "lib/hal/HalDictionaryRemovalStorage.h"
using namespace companion;
std::array<uint8_t, 256> scratch{};
HalDictionaryCacheStorage cache;
HalDictionaryBindings bindings(cache, scratch);
void SetUp() { inventory_hal_test::state = {}; }
ContentManifest archive(bool compressed, bool comment) {
  std::ifstream file(std::string(COMPANION_FIXTURE_DIR) +
                         (compressed ? "/DictionaryBundle-dictzip.fixture" : "/DictionaryBundle-plain.fixture"),
                     std::ios::binary);
  std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
  assert(!bytes.empty());
  if (comment) {
    bytes[bytes.size() - 2] = 3;
    bytes.insert(bytes.end(), {'z', 'i', 'p'});
  }
  inventory_hal_test::state.files[DICTIONARY_CACHE_CANDIDATE] = bytes;
  ContentManifest manifest;
  manifest.kind = ContentKind::Dictionary;
  manifest.formatVersion = 1;
  manifest.length = bytes.size();
  EVP_Digest(bytes.data(), bytes.size(), manifest.contentHash.data(), nullptr, EVP_sha256(), nullptr);
  DictionaryCachePublication publication(cache, scratch);
  assert(publication.publish(manifest) == DictionaryCacheResult::Ok);
  return manifest;
}
DictionaryArchiveBinding value(bool compressed = false) {
  return {archive(compressed, false), archive(compressed, true)};
}
DictionaryRemovalPlan removalPlan(const DictionaryArchiveBinding& binding) {
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
std::string target(const char* path = "/dictionaries/es/stem") {
  Digest hash{};
  EVP_Digest(path, strlen(path), hash.data(), nullptr, EVP_sha256(), nullptr);
  std::string result = "/.crosspoint/companion/dictionary-binding-";
  result.reserve(110);
  constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (auto byte : hash) {
    result += HEX_DIGITS[byte >> 4];
    result += HEX_DIGITS[byte & 15];
  }
  return result;
}
namespace {
struct DictionarySettings final : DictionaryRemovalSettings {
  std::string selected, persisted;
  bool invalid = false, failSave = false;
  unsigned saves = 0;
  bool load() override {
    selected = persisted;
    return true;
  }
  bool selectedDirectory(std::string_view& output) const override {
    if (invalid) return false;
    output = selected;
    return true;
  }
  bool clearAndSave() override {
    selected.clear();
    return save();
  }
  bool save() override {
    ++saves;
    if (failSave) return false;
    persisted = selected;
    return true;
  }
};
}  // namespace
void DictionaryReferencesRecoverFailedSelectionSaveAndRetireOnlyBindingAndOwnedMembers() {
  for (bool reconstruct : {false, true}) {
    SetUp();
    const auto binding = value();
    auto plan = removalPlan(binding);
    auto& state = inventory_hal_test::state;
    state.enumerateFileMap = true;
    state.directories["/"] = {};
    state.directories["/dictionaries"] = {};
    state.directories["/dictionaries/es"] = {};
    std::array<char, 144> path{};
    for (unsigned member = 0; member < 3; ++member) {
      const std::vector<uint8_t> data(100 * (member + 1), static_cast<uint8_t>(member + 5));
      EVP_Digest(data.data(), data.size(), plan.installed.extraction.hashes[member].data(), nullptr, EVP_sha256(),
                 nullptr);
      assert(dictionaryRemovalMemberPath(plan, member, path));
      state.files[path.data()] = data;
    }
    state.files["/dictionaries/es/notes.txt"] = {9};
    assert(bindings.install(plan.installed.base.data(), binding));
    assert(bindings.finalizeInstallation(plan.installed.base.data(), binding));
    std::array<uint8_t, DICTIONARY_REMOVAL_PLAN_SIZE> bytes{};
    assert((DictionaryRemovalPlanCodec::encode(plan, bytes)) == (bytes.size()));
    ContentRemovalRecord initial;
    initial.request = plan.request;
    EVP_Digest(bytes.data(), bytes.size(), initial.planHash.data(), nullptr, EVP_sha256(), nullptr);
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
    HalContentRemovalJournalStorage storage;
    ContentRemovalJournal journal(storage, journalScratch);
    DictionarySettings settings;
    settings.selected = settings.persisted = "ES";
    settings.failSave = true;
    HalDictionaryRemovalReferences refs(journal, settings, bindings);
    HalDictionaryRemovalStorage members(journal, bytes, scratch);
    DictionaryRemovalParticipant participant(journal, members, refs);
    assert(participant.bind(plan, initial.planHash));
    ContentRemoval removal(journal, participant);
    assert((removal.remove(initial)) == (ContentRemovalJournalResult::IoError));
    assert((journal.current()->phase) == (ContentRemovalPhase::Quarantined));
    assert(settings.selected.empty());
    assert((settings.persisted) == ("ES"));
    assert(state.files.contains(target()));
    assert((std::count_if(state.files.begin(), state.files.end(), [](const auto& entry) {
             return entry.first.starts_with("/.crosspoint/companion/removal-cohort-bytes-");
           })) == (3));
    settings.failSave = false;
    if (reconstruct) {
      assert(settings.load());
      ContentRemovalJournal recovered(storage, journalScratch);
      HalDictionaryBindings recoveredBindings(cache, scratch);
      HalDictionaryRemovalReferences recoveredRefs(recovered, settings, recoveredBindings);
      HalDictionaryRemovalStorage recoveredMembers(recovered, bytes, scratch);
      DictionaryRemovalParticipant recoveredParticipant(recovered, recoveredMembers, recoveredRefs);
      assert(recoveredParticipant.bind(plan, initial.planHash));
      ContentRemoval retry(recovered, recoveredParticipant);
      assert((retry.remove(initial)) == (ContentRemovalJournalResult::Ok));
    } else {
      assert((removal.remove(initial)) == (ContentRemovalJournalResult::Ok));
    }
    assert(settings.persisted.empty());
    assert((settings.saves) == (2U));
    assert(!(state.files.contains(target())));
    assert((state.files.at("/dictionaries/es/notes.txt")) == (std::vector<uint8_t>({9})));
    for (unsigned member = 0; member < 3; ++member) {
      assert(dictionaryRemovalMemberPath(plan, member, path));
      assert(!(state.files.contains(path.data())));
    }
    DictionaryCachePublication publication(cache, scratch);
    assert((publication.find(binding.members)) == (DictionaryCacheResult::Ok));
    assert((publication.find(binding.original)) == (DictionaryCacheResult::Ok));
  }
}
void DictionaryReferencePreflightRefusesInvalidSettingsOrForeignBindingWithoutSaving() {
  const auto binding = value();
  const auto plan = removalPlan(binding);
  assert(bindings.install(plan.installed.base.data(), binding));
  assert(bindings.finalizeInstallation(plan.installed.base.data(), binding));
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal(storage, journalScratch);
  ContentRemovalRecord initial;
  initial.request = plan.request;
  initial.planHash.fill(8);
  DictionarySettings settings;
  settings.selected = settings.persisted = "es";
  settings.invalid = true;
  HalDictionaryRemovalReferences refs(journal, settings, bindings);
  const auto before = inventory_hal_test::state.files;
  assert(!(refs.verifyPlan(initial, plan)));
  assert((settings.saves) == (0U));
  assert((inventory_hal_test::state.files) == (before));
  settings.invalid = false;
  assert(refs.verifyPlan(initial, plan));
  const auto foreign = value(true);
  assert(bindings.install(plan.installed.base.data(), foreign));
  assert(bindings.finalizeInstallation(plan.installed.base.data(), foreign));
  assert(!(refs.verifyPlan(initial, plan)));
  assert((settings.saves) == (0U));
  assert((settings.persisted) == ("es"));
}
void DictionaryReferencePublicationPreservesOtherSelectionAndAvoidsRepeatedSaves() {
  const auto binding = value();
  const auto plan = removalPlan(binding);
  assert(bindings.install(plan.installed.base.data(), binding));
  assert(bindings.finalizeInstallation(plan.installed.base.data(), binding));
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal(storage, journalScratch);
  ContentRemovalRecord initial;
  initial.request = plan.request;
  initial.planHash.fill(8);
  DictionarySettings settings;
  settings.selected = settings.persisted = "fr";
  HalDictionaryRemovalReferences refs(journal, settings, bindings);
  assert(refs.verifyPlan(initial, plan));
  assert((journal.begin(initial)) == (ContentRemovalJournalResult::Ok));
  assert((journal.advance(ContentRemovalPhase::Quarantined)) == (ContentRemovalJournalResult::Ok));
  const auto record = *journal.current();
  assert(refs.publish(record, plan));
  assert(refs.publish(record, plan));
  assert((settings.saves) == (1U));
  assert((settings.persisted) == ("fr"));
  assert(refs.verify(record, plan));
  assert(!(refs.retire(record, plan)));
  assert(inventory_hal_test::state.files.contains(target()));
}

void HiddenAndVisibleUnicodeFolderSelectionsUseSdFatComparison() {
  for (bool hidden : {false, true}) {
    SetUp();
    const auto binding = value();
    auto plan = removalPlan(binding);
    std::strcpy(plan.installed.base.data(), hidden ? "/.dictionaries/café/stem" : "/dictionaries/café/stem");
    assert(bindings.install(plan.installed.base.data(), binding));
    assert(bindings.finalizeInstallation(plan.installed.base.data(), binding));
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
    HalContentRemovalJournalStorage storage;
    ContentRemovalJournal journal(storage, journalScratch);
    ContentRemovalRecord initial;
    initial.request = plan.request;
    initial.planHash.fill(8);
    DictionarySettings settings;
    settings.selected = settings.persisted = "CAFÉ";
    HalDictionaryRemovalReferences refs(journal, settings, bindings);
    assert(refs.verifyPlan(initial, plan));
    assert(journal.begin(initial) == ContentRemovalJournalResult::Ok);
    assert(journal.advance(ContentRemovalPhase::Quarantined) == ContentRemovalJournalResult::Ok);
    assert(refs.publish(*journal.current(), plan));
    assert(settings.persisted.empty());
    assert(settings.saves == 1);
    assert(refs.verify(*journal.current(), plan));
  }
}

int main() {
  HiddenAndVisibleUnicodeFolderSelectionsUseSdFatComparison();
  SetUp();
  DictionaryReferencesRecoverFailedSelectionSaveAndRetireOnlyBindingAndOwnedMembers();
  SetUp();
  DictionaryReferencePreflightRefusesInvalidSettingsOrForeignBindingWithoutSaving();
  SetUp();
  DictionaryReferencePublicationPreservesOtherSelectionAndAvoidsRepeatedSaves();
}

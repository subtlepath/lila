#include <openssl/evp.h>

#include <cassert>
#include <fstream>
#include <iterator>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalDictionaryCacheStorage.h"
#include "lib/hal/HalDictionaryRemovalCohortParticipant.h"
#include "lib/hal/HalDictionaryRemovalPlanCollection.h"
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

std::vector<uint8_t> fixture(const char* name) {
  std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}
void populateFolder(const std::string& folder) {
  auto& state = inventory_hal_test::state;
  state.directories[folder] = {};
  state.files[folder + "/stem.dict"] = {'o', 'n', 'e', 't', 'w', 'o'};
  state.files[folder + "/stem.idx"] = fixture("DictionaryIndex-definitions.fixture");
  const std::string info =
      "StarDict's dict ifo file\nversion=3.0.0\nbookname=Canonical "
      "dictionary\nwordcount=2\nidxfilesize=24\nsynwordcount=1\n";
  state.files[folder + "/stem.ifo"] = {info.begin(), info.end()};
  state.files[folder + "/stem.syn"] = fixture("DictionaryIndex-synonyms.fixture");
  state.files[folder + "/notes.txt"] = {90};
}
int main() {
  for (unsigned fault = 0; fault < 11; ++fault) {
    SetUp();
    const auto binding = value();
    auto& state = inventory_hal_test::state;
    state.enumerateFileMap = true;
    state.directories["/"] = {};
    state.directories["/dictionaries"] = {};
    state.directories["/.dictionaries"] = {};
    populateFolder("/.dictionaries/café");
    populateFolder("/dictionaries/A");
    populateFolder("/dictionaries/A!");
    populateFolder("/dictionaries/Bound");
    assert(bindings.install("/dictionaries/Bound/stem", binding));
    assert(bindings.finalizeInstallation("/dictionaries/Bound/stem", binding));
    state.directories["/dictionaries/empty"] = {};
    state.directories["/dictionaries/.ignored"] = {};
    state.files["/dictionaries/.ignored/broken.idx"] = {1};
    const auto request = removalPlan(binding).request;
    auto selectedRequest = request;
    selectedRequest.manifest = binding.members;
    if (fault == 1) state.directoryErrorPath = "/";
    if (fault == 2) state.directoryErrorPath = "/dictionaries";
    if (fault == 3) state.files["/dictionaries/A/stem.dict"][0] ^= 1;
    if (fault == 4) state.directories["/dictionaries/a"] = {};
    if (fault == 5) {
      state.directories["/dictionaries/Other"] = {};
      state.aliases["/dictionaries/Other"] = "A";
    }
    if (fault == 6) {
      state.directories.erase("/dictionaries");
      state.files["/dictionaries"] = {1};
    }
    if (fault == 7) state.failClosePath = "/dictionaries";
    if (fault == 8) selectedRequest.manifest.contentHash.fill(55);
    struct Guard {
      unsigned calls = 0, cancelAt = UINT32_MAX;
    } guard;
    if (fault == 9) guard.cancelAt = 1;
    if (fault == 10) guard.cancelAt = 150;
    auto progress = [](void* context) {
      auto& g = *static_cast<Guard*>(context);
      return ++g.calls < g.cancelAt;
    };
    HalDictionaryRemovalPlanAssembly assembly(cache, scratch, progress, &guard);
    HalDictionaryRemovalCohortPlanWriter writer(65536);
    HalDictionaryRemovalPlanCollection collection(assembly, writer, progress, &guard);
    const auto ownedInputs = state.files;
    const bool collected = collection.collect(selectedRequest, 7);
    assert(collected == (fault == 0));
    for (const auto& [path, bytes] : ownedInputs) assert(state.files.at(path) == bytes);
    assert(!state.files.contains(CONTENT_REMOVAL_JOURNALS[0]));
    if (collected) {
      HalDictionaryRemovalCohortPlanStorage plans;
      assert(plans.open(*writer.publishedDigest(), selectedRequest) == DictionaryRemovalCohortStorageResult::Ok);
      assert(plans.current()->count == 3);
      DictionaryRemovalPlan output;
      for (const char* path : {"/.dictionaries/café/stem", "/dictionaries/A!/stem", "/dictionaries/A/stem"}) {
        assert(plans.next(output) == InventoryPathRecordResult::Entry);
        assert(std::strcmp(output.installed.base.data(), path) == 0);
        assert(output.request == selectedRequest);
      }
      assert(plans.next(output) == InventoryPathRecordResult::End);
      assert(guard.calls > 150);
      std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
      HalContentRemovalJournalStorage journalStorage;
      ContentRemovalJournal journal(journalStorage, journalScratch);
      DictionarySettings settings;
      settings.selected = settings.persisted = "CAFÉ";
      HalDictionaryRemovalReferences references(journal, settings, bindings);
      HalDictionaryRemovalCohortParticipant participant(journal, plans, references, scratch);
      ContentRemovalRecord initial;
      initial.request = selectedRequest;
      initial.planHash = *writer.publishedDigest();
      ContentRemoval removal(journal, participant);
      assert(removal.remove(initial) == ContentRemovalJournalResult::Ok);
      assert(settings.persisted.empty());
      assert(settings.saves == 1);
      for (const char* folder : {"/.dictionaries/café", "/dictionaries/A!", "/dictionaries/A"}) {
        for (const char* suffix : {".dict", ".idx", ".ifo", ".syn"})
          assert(!state.files.contains(std::string(folder) + "/stem" + suffix));
        assert(state.files.at(std::string(folder) + "/notes.txt") == std::vector<uint8_t>({90}));
      }
      for (const auto& [path, bytes] : ownedInputs) {
        if (path.starts_with("/dictionaries/Bound/") || path.starts_with("/.crosspoint/companion/dictionary-"))
          assert(state.files.at(path) == bytes);
      }
      const auto retained = state.files;
      assert(removal.remove(initial) == ContentRemovalJournalResult::Ok);
      assert(state.files == retained);
      assert(settings.saves == 1);
    } else {
      assert(!writer.publishedDigest());
      if (fault == 10) {
        std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> claimScratch{};
        HalRemovalStageClaimStorage claim(claimScratch);
        assert(claim.load({selectedRequest, 7}) == RemovalStageClaimResult::Ok);
        assert(state.files.contains(claim.planStagePath()));
        const auto retainedStage = state.files.at(claim.planStagePath());
        assert(writer.releaseHandles());
        assert(state.files.at(claim.planStagePath()) == retainedStage);
      }
    }
  }
}

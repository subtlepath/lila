#include <openssl/evp.h>

#include <cassert>
#include <fstream>
#include <iterator>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalDictionaryCacheStorage.h"
#include "lib/hal/HalDictionaryRemovalCohortParticipant.h"
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

struct ReferenceFault final : DictionaryRemovalReferences {
  DictionaryRemovalReferences& inner;
  std::string corruptBackup;
  explicit ReferenceFault(DictionaryRemovalReferences& inner) : inner(inner) {}
  bool verifyPlan(const ContentRemovalRecord& r, const DictionaryRemovalPlan& p) override {
    return inner.verifyPlan(r, p);
  }
  bool publish(const ContentRemovalRecord& r, const DictionaryRemovalPlan& p) override { return inner.publish(r, p); }
  bool verify(const ContentRemovalRecord& r, const DictionaryRemovalPlan& p) override { return inner.verify(r, p); }
  bool retire(const ContentRemovalRecord& r, const DictionaryRemovalPlan& p) override {
    const bool result = inner.retire(r, p);
    if (result && !corruptBackup.empty()) {
      inventory_hal_test::state.files.at(corruptBackup)[0] ^= 1;
      corruptBackup.clear();
    }
    return result;
  }
  bool verifyRetired(const ContentRemovalRecord& r, const DictionaryRemovalPlan& p) override {
    return inner.verifyRetired(r, p);
  }
};
std::string parentAddress(const Digest& digest) {
  std::string result = "/.crosspoint/companion/removal-dictionary-plan-";
  result.reserve(128);
  static constexpr char DIGITS[] = "0123456789abcdef";
  for (auto byte : digest) {
    result += DIGITS[byte >> 4];
    result += DIGITS[byte & 15];
  }
  return result;
}
int main() {
  for (unsigned fault = 0; fault < 8; ++fault) {
    SetUp();
    const auto binding = value();
    auto first = removalPlan(binding);
    first.installed.extraction.synonyms = true;
    first.installed.extraction.compressed = true;
    first.installed.extraction.sealed = first.installed.published = 15;
    first.installed.extraction.lengths = {100, 200, 300, 400};
    for (unsigned member = 0; member < 4; ++member) {
      const std::vector<uint8_t> data(100 * (member + 1), static_cast<uint8_t>(member + 5));
      EVP_Digest(data.data(), data.size(), first.installed.extraction.hashes[member].data(), nullptr, EVP_sha256(),
                 nullptr);
    }
    auto second = first;
    std::strcpy(first.installed.base.data(), "/.dictionaries/es/stem");
    auto& state = inventory_hal_test::state;
    state.enumerateFileMap = true;
    state.directories["/"] = {};
    state.directories["/dictionaries"] = {};
    state.directories["/.dictionaries"] = {};
    state.directories["/dictionaries/es"] = {};
    state.directories["/.dictionaries/es"] = {};
    std::array<char, 144> path{};
    std::array<std::string, 8> paths;
    for (unsigned copy = 0; copy < 2; ++copy) {
      const auto& plan = copy ? second : first;
      for (unsigned member = 0; member < 4; ++member) {
        assert(dictionaryRemovalMemberPath(plan, member, path));
        paths[copy * 4 + member] = path.data();
        state.files[path.data()] = std::vector<uint8_t>(100 * (member + 1), static_cast<uint8_t>(member + 5));
      }
      assert(bindings.install(plan.installed.base.data(), binding));
      assert(bindings.finalizeInstallation(plan.installed.base.data(), binding));
    }
    state.files["/dictionaries/es/notes.txt"] = {90};
    state.files["/.dictionaries/es/notes.txt"] = {91};
    std::vector<uint8_t> bytes(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + 2 * DICTIONARY_REMOVAL_PLAN_SIZE);
    auto payload = std::span(bytes).subspan(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE);
    assert(DictionaryRemovalPlanCodec::encode(first, payload.first(DICTIONARY_REMOVAL_PLAN_SIZE)) ==
           DICTIONARY_REMOVAL_PLAN_SIZE);
    assert(DictionaryRemovalPlanCodec::encode(second, payload.subspan(DICTIONARY_REMOVAL_PLAN_SIZE)) ==
           DICTIONARY_REMOVAL_PLAN_SIZE);
    DictionaryRemovalCohortHeader header{first.request, 7, 2, inventoryIndexCrc(payload)};
    assert(encodeDictionaryRemovalCohortHeader(header, bytes) == DICTIONARY_REMOVAL_COHORT_HEADER_SIZE);
    ContentRemovalRecord initial;
    initial.request = first.request;
    EVP_Digest(bytes.data(), bytes.size(), initial.planHash.data(), nullptr, EVP_sha256(), nullptr);
    const auto parentPath = parentAddress(initial.planHash);
    state.files[parentPath] = bytes;
    std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalScratch{};
    HalContentRemovalJournalStorage storage;
    ContentRemovalJournal journal(storage, journalScratch);
    HalDictionaryRemovalCohortPlanStorage plans;
    assert(plans.open(initial.planHash, first.request) == DictionaryRemovalCohortStorageResult::Ok);
    DictionarySettings settings;
    settings.selected = settings.persisted = "ES";
    HalDictionaryRemovalReferences nativeRefs(journal, settings, bindings);
    ReferenceFault refs(nativeRefs);
    HalDictionaryRemovalCohortParticipant participant(journal, plans, refs, scratch);
    ContentRemoval removal(journal, participant);
    const auto renames = state.renames;
    if (fault == 1) state.failRename = state.renames + 5;
    if (fault == 2) state.failRenameAfter = state.renames + 5;
    if (fault == 3) settings.failSave = true;
    if (fault == 4) state.failRemoveAfter = true;
    if (fault == 5) state.files[paths[6]][0] ^= 1;
    std::array<char, 112> corruptPath{};
    if (fault == 6) {
      mbedtls_sha256_context hash;
      mbedtls_sha256_init(&hash);
      assert(removalCohortAddress(hash, initial.planHash, 6, corruptPath));
      mbedtls_sha256_free(&hash);
      refs.corruptBackup = corruptPath.data();
    }
    if (fault == 7) state.failClosePath = paths[6];
    const auto result = removal.remove(initial);
    assert((result == ContentRemovalJournalResult::Ok) == (fault == 0));
    if (fault == 5 || fault == 7) {
      assert(state.renames == renames);
      assert(!journal.current());
      for (const auto& path : paths) assert(state.files.contains(path));
    }
    if (fault == 6) {
      assert(journal.current()->phase == ContentRemovalPhase::Committed);
      assert(std::count_if(state.files.begin(), state.files.end(), [](const auto& entry) {
               return entry.first.starts_with("/.crosspoint/companion/removal-cohort-bytes-");
             }) == 8);
      state.files.at(corruptPath.data())[0] ^= 1;
    }
    if (fault == 5) state.files[paths[6]][0] ^= 1;
    state.failRename = state.failRenameAfter = 0;
    state.failRemoveAfter = false;
    state.failClosePath.clear();
    settings.failSave = false;
    assert(settings.load());
    assert(plans.close());
    ContentRemovalJournal recovered(storage, journalScratch);
    HalDictionaryBindings recoveredBindings(cache, scratch);
    HalDictionaryRemovalReferences recoveredRefs(recovered, settings, recoveredBindings);
    HalDictionaryRemovalCohortPlanStorage recoveredPlans;
    assert(recoveredPlans.open(initial.planHash, first.request) == DictionaryRemovalCohortStorageResult::Ok);
    HalDictionaryRemovalCohortParticipant resumed(recovered, recoveredPlans, recoveredRefs, scratch);
    ContentRemoval retry(recovered, resumed);
    assert(retry.remove(initial) == ContentRemovalJournalResult::Ok);
    assert(recovered.current()->phase == ContentRemovalPhase::Retired);
    for (const auto& path : paths) assert(!state.files.contains(path));
    assert(!state.files.contains(target(first.installed.base.data())));
    assert(!state.files.contains(target(second.installed.base.data())));
    assert(settings.persisted.empty());
    assert(settings.saves == (fault == 3 ? 2U : 1U));
    assert(state.files.at("/dictionaries/es/notes.txt") == std::vector<uint8_t>({90}));
    assert(state.files.at("/.dictionaries/es/notes.txt") == std::vector<uint8_t>({91}));
    assert(state.files.at(parentPath) == bytes);
    const auto retained = state.files;
    const auto saves = settings.saves;
    assert(retry.remove(initial) == ContentRemovalJournalResult::Ok);
    assert(state.files == retained);
    assert(settings.saves == saves);
  }
}

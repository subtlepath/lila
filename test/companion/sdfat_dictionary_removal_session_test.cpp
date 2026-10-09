#include <openssl/evp.h>

#include <cassert>
#include <fstream>
#include <iterator>

#include "lib/hal/HalContentRemovalJournalStorage.h"
#include "lib/hal/HalContentRemovalStartupRecovery.h"
#include "lib/hal/HalDictionaryCacheStorage.h"
#include "lib/hal/HalDictionaryRemovalCohortParticipant.h"
#include "lib/hal/HalDictionaryRemovalNativeOwner.h"
#include "lib/hal/HalDictionaryRemovalPlanCollection.h"
#include "lib/hal/HalDictionaryRemovalReferences.h"
#include "lib/hal/HalDictionaryRemovalSession.h"
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
  bool invalid = false, failSave = false, failLoad = false;
  unsigned saves = 0;
  bool load() override {
    if (failLoad) return false;
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

struct PathsMemory final : InventoryIndexStorage {
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
void encodePaths(PathsMemory& memory, const ContentRemovalRequest& request, bool empty = false,
                 bool mismatched = false) {
  memory.bytes.resize(INVENTORY_INDEX_HEADER_SIZE + INVENTORY_PATH_MAX_RECORD);
  size_t length = INVENTORY_INDEX_HEADER_SIZE;
  auto manifest = request.manifest;
  if (mismatched) ++manifest.length;
  if (!empty)
    length += encodeInventoryPath(manifest, "/.crosspoint/companion/dictionary-cached.zip",
                                  std::span(memory.bytes).subspan(length));
  memory.bytes.resize(length);
  InventoryIndexHeader header{request.generation, 7, empty ? 0U : 1U,
                              inventoryIndexCrc(std::span(memory.bytes).subspan(INVENTORY_INDEX_HEADER_SIZE))};
  assert(encodeInventoryPathsHeader(header, memory.bytes) == INVENTORY_INDEX_HEADER_SIZE);
}
int main() {
  {
    SetUp();
    DictionarySettings settings;
    const auto generation = removalPlan(value()).request.generation;
    auto owner = std::make_unique<HalDictionaryRemovalNativeOwner>(
        generation, 65536, settings, [](void*) { return true; }, [](void*) { return true; }, nullptr, nullptr);
    assert(!owner->openInventory(0));
    assert(!owner->openInventory(7));
    auto request = removalPlan(value()).request;
    request.manifest = value().members;
    PathsMemory publishedPaths;
    encodePaths(publishedPaths, request);
    auto& files = inventory_hal_test::state.files;
    files[InventoryPublication::PATHS] = publishedPaths.bytes;
    std::vector<uint8_t> index(INVENTORY_INDEX_HEADER_SIZE + INVENTORY_PATH_MAX_RECORD);
    const auto entryLength =
        encodeInventoryIndexEntry(request.manifest, std::span(index).subspan(INVENTORY_INDEX_HEADER_SIZE));
    assert(entryLength);
    index.resize(INVENTORY_INDEX_HEADER_SIZE + entryLength);
    InventoryIndexHeader header{generation, 7, 1,
                                inventoryIndexCrc(std::span(index).subspan(INVENTORY_INDEX_HEADER_SIZE))};
    assert(encodeInventoryIndexHeader(header, index) == INVENTORY_INDEX_HEADER_SIZE);
    files[InventoryPublication::INDEX] = index;
    assert(owner->openInventory(7));
    assert(owner->closeReaders());
    assert(!owner->openInventory(8));
    files[InventoryPublication::PATHS].back() ^= 1;
    assert(!owner->openInventory(7));
    files[InventoryPublication::PATHS] = publishedPaths.bytes;
    assert(owner->openInventory(7));
    assert(!owner->setPlanQuota(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE - 1));
    assert(owner->setPlanQuota(65536));
    assert(owner->prepare());
    assert(owner->closeReaders());
    assert(owner->closeReaders());
  }
  for (unsigned fault = 0; fault < 5; ++fault) {
    SetUp();
    const auto binding = value();
    auto request = removalPlan(binding).request;
    request.manifest = binding.members;
    auto& state = inventory_hal_test::state;
    state.enumerateFileMap = true;
    state.directories["/"] = {};
    state.directories["/dictionaries"] = {};
    state.directories["/.dictionaries"] = {};
    populateFolder("/dictionaries/es");
    populateFolder("/.dictionaries/es");
    PathsMemory memory;
    encodePaths(memory, request, fault == 3, fault == 4);
    std::array<uint8_t, INVENTORY_PATH_MAX_RECORD> pathScratch{};
    InventoryPaths paths(memory, pathScratch);
    const uint64_t revision = 7;
    DictionarySettings settings;
    settings.selected = settings.persisted = "ES";
    struct Context {
      bool permitted = true, inventory = true;
      unsigned refreshes = 0, inventoryCalls = 0;
    } context;
    auto permitted = [](void* context) { return static_cast<Context*>(context)->permitted; };
    auto refresh = [](void* context) {
      ++static_cast<Context*>(context)->refreshes;
      return true;
    };
    auto ready = [](void* context, uint64_t& revision) {
      auto& state = *static_cast<Context*>(context);
      ++state.inventoryCalls;
      revision = 7;
      return state.inventory;
    };
    auto session = std::make_unique<HalDictionaryRemovalSession>(request.generation, paths, revision, 65536, scratch,
                                                                 settings, permitted, refresh, &context, ready);
    std::array<uint8_t, CONTENT_REMOVAL_REQUEST_SIZE> body{};
    std::array<uint8_t, CONTENT_REMOVAL_REPLY_SIZE> reply{};
    assert(encodeContentRemovalRequest(request, body) == body.size());
    const auto unprepared = state.files;
    reply.fill(0xa5);
    assert(session->handle(true, request.owner, body, reply) == 0);
    assert(std::all_of(reply.begin(), reply.end(), [](uint8_t value) { return value == 0xa5; }));
    assert(state.files == unprepared);
    assert(context.inventoryCalls == 0);
    assert(settings.saves == 0);
    assert(session->prepare());
    const auto before = state.files;
    auto unsupported = request;
    unsupported.manifest.kind = ContentKind::Epub;
    assert(encodeContentRemovalRequest(unsupported, body) == body.size());
    assert(session->handle(true, request.owner, body, reply) == reply.size());
    assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Unsupported));
    assert(state.files == before);
    assert(context.inventoryCalls == 0);
    assert(settings.saves == 0);
    assert(encodeContentRemovalRequest(request, body) == body.size());
    assert(session->handle(false, request.owner, body, reply) == reply.size());
    assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Unauthorized));
    assert(state.files == before);
    auto wrong = request.owner;
    wrong.fill(9);
    assert(session->handle(true, wrong, body, reply) == reply.size());
    assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Unauthorized));
    assert(state.files == before);
    context.permitted = false;
    assert(session->handle(true, request.owner, body, reply) == reply.size());
    assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Busy));
    assert(state.files == before);
    context.permitted = true;
    if (fault == 1) state.failRenameAfter = state.renames + 3;
    if (fault == 2) settings.failSave = true;
    assert(session->handle(true, request.owner, body, reply) == reply.size());
    if (!fault)
      assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Ok));
    else
      assert(reply[0] != static_cast<uint8_t>(ContentRemovalResult::Ok));
    assert(std::equal(request.transaction.begin(), request.transaction.end(), reply.begin() + 1));
    if (fault == 3 || fault == 4) {
      assert(reply[0] ==
             static_cast<uint8_t>(fault == 3 ? ContentRemovalResult::NotFound : ContentRemovalResult::Conflict));
      assert(state.files == before);
      assert(settings.saves == 0);
      assert(context.refreshes == 0);
      continue;
    }
    state.failRenameAfter = 0;
    settings.failSave = false;
    assert(session->closeReaders());
    session.reset();
    if (fault == 1) {
      const auto pendingFiles = state.files;
      auto unavailable = std::make_unique<HalContentRemovalStartupRecovery>();
      assert(!unavailable->run(request.generation));
      assert(state.files == pendingFiles);
      auto startup = std::make_unique<HalContentRemovalStartupRecovery>(nullptr, &settings);
      settings.failLoad = true;
      assert(!startup->run(request.generation));
      assert(state.files == pendingFiles);
      assert(settings.saves == 0);
      settings.failLoad = false;
      auto planFile = std::find_if(state.files.begin(), state.files.end(), [](const auto& entry) {
        return entry.first.starts_with("/.crosspoint/companion/removal-dictionary-plan-");
      });
      assert(planFile != state.files.end() && !planFile->second.empty());
      const auto savedPlan = planFile->second;
      planFile->second.back() ^= 1;
      const auto corruptFiles = state.files;
      assert(!startup->run(request.generation));
      assert(state.files == corruptFiles);
      assert(settings.saves == 0);
      planFile->second = savedPlan;
      assert(startup->run(request.generation));
      assert(startup->run(request.generation));
      assert(settings.persisted.empty());
      assert(settings.saves == 1);
    }
    assert(settings.load());
    encodePaths(memory, request, true);
    context.inventory = false;
    const auto readyCalls = context.inventoryCalls;
    auto recovered = std::make_unique<HalDictionaryRemovalSession>(request.generation, paths, revision, 65536, scratch,
                                                                   settings, permitted, refresh, &context, ready);
    assert(recovered->prepare());
    assert(recovered->handle(true, request.owner, body, reply) == reply.size());
    assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Ok));
    assert(context.inventoryCalls == readyCalls);
    assert(settings.persisted.empty());
    assert(settings.saves == (fault == 2 ? 2U : 1U));
    for (const char* root : {"/dictionaries/es", "/.dictionaries/es"}) {
      for (const char* suffix : {".dict", ".idx", ".ifo", ".syn"})
        assert(!state.files.contains(std::string(root) + "/stem" + suffix));
      assert(state.files.at(std::string(root) + "/notes.txt") == std::vector<uint8_t>({90}));
    }
    const auto retired = state.files;
    const auto saves = settings.saves;
    assert(recovered->handle(true, request.owner, body, reply) == reply.size());
    assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Ok));
    assert(state.files == retired);
    assert(settings.saves == saves);
    auto conflict = request;
    ++conflict.manifest.length;
    assert(encodeContentRemovalRequest(conflict, body) == body.size());
    assert(recovered->handle(true, request.owner, body, reply) == reply.size());
    assert(reply[0] == static_cast<uint8_t>(ContentRemovalResult::Conflict));
    assert(state.files == retired);
    assert(recovered->closeReaders());
  }
}

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <fstream>
#include <iterator>

#include "lib/hal/HalDictionaryCacheStorage.h"
#include "lib/hal/HalDictionaryRemovalPlanAssembly.h"
using namespace companion;
namespace {
class DictionaryRemovalPlanAssemblyTest : public testing::Test {
 protected:
  std::array<uint8_t, 256> scratch{};
  HalDictionaryCacheStorage cache;
  static std::vector<uint8_t> fixture(const char* name) {
    std::ifstream file(std::string(COMPANION_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  ContentManifest populate(bool compressed = false, bool hidden = false) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    const std::string root = hidden ? "/.dictionaries" : "/dictionaries";
    state.directories["/"] = {};
    state.directories[root] = {};
    state.directories[root + "/es"] = {};
    state.files[root + "/es/stem.idx"] = fixture("DictionaryIndex-definitions.fixture");
    const std::string info =
        "StarDict's dict ifo file\nversion=3.0.0\nbookname=Canonical "
        "dictionary\nwordcount=2\nidxfilesize=24\nsynwordcount=1\n";
    state.files[root + "/es/stem.ifo"] = {info.begin(), info.end()};
    state.files[root + "/es/stem.syn"] = fixture("DictionaryIndex-synonyms.fixture");
    if (compressed)
      state.files[root + "/es/stem.dict.dz"] = fixture("dictzip-chunks.dict.dz");
    else
      state.files[root + "/es/stem.dict"] = {'o', 'n', 'e', 't', 'w', 'o'};
    state.files[root + "/es/notes.txt"] = {90};
    return publish(fixture(compressed ? "DictionaryBundle-dictzip.fixture" : "DictionaryBundle-plain.fixture"));
  }
  ContentManifest publish(const std::vector<uint8_t>& bytes) {
    inventory_hal_test::state.files[DICTIONARY_CACHE_CANDIDATE] = bytes;
    ContentManifest manifest;
    manifest.kind = ContentKind::Dictionary;
    manifest.formatVersion = 1;
    manifest.length = bytes.size();
    EVP_Digest(bytes.data(), bytes.size(), manifest.contentHash.data(), nullptr, EVP_sha256(), nullptr);
    DictionaryCachePublication publication(cache, scratch);
    EXPECT_EQ(publication.publish(manifest), DictionaryCacheResult::Ok);
    return manifest;
  }
  static ContentRemovalRequest request(const ContentManifest& manifest) {
    ContentRemovalRequest result;
    result.manifest = manifest;
    result.transaction.fill(1);
    result.owner.fill(2);
    result.generation.fill(3);
    return result;
  }
};
}  // namespace
TEST_F(DictionaryRemovalPlanAssemblyTest, LegacyAndCompressedBundlesProduceExactMemberProofsWithoutWrites) {
  for (bool compressed : {false, true})
    for (bool hidden : {false, true}) {
      const auto manifest = populate(compressed, hidden);
      const std::string folder = hidden ? "/.dictionaries/es" : "/dictionaries/es";
      const auto before = inventory_hal_test::state.files;
      HalDictionaryRemovalPlanAssembly assembly(cache, scratch);
      DictionaryRemovalPlan output;
      ASSERT_EQ(assembly.assemble(folder, request(manifest), output), DictionaryRemovalAssemblyResult::Match);
      EXPECT_TRUE(validDictionaryRemovalPlan(output));
      EXPECT_EQ(output.request, request(manifest));
      EXPECT_EQ(output.installed.archives.members, manifest);
      EXPECT_EQ(output.installed.archives.original, manifest);
      EXPECT_EQ(output.installed.extraction.compressed, compressed);
      EXPECT_TRUE(output.installed.extraction.synonyms);
      for (unsigned member = 0; member < 4; ++member) {
        std::array<char, 144> path{};
        ASSERT_TRUE(dictionaryRemovalMemberPath(output, member, path));
        const auto& bytes = before.at(path.data());
        Digest hash{};
        EVP_Digest(bytes.data(), bytes.size(), hash.data(), nullptr, EVP_sha256(), nullptr);
        EXPECT_EQ(output.installed.extraction.lengths[member], bytes.size());
        EXPECT_EQ(output.installed.extraction.hashes[member], hash);
      }
      EXPECT_EQ(inventory_hal_test::state.files, before);
    }
}
TEST_F(DictionaryRemovalPlanAssemblyTest, OriginalArchiveBindingIsRetainedAndForeignRequestsLeaveOutputUntouched) {
  const auto canonical = populate();
  auto bytes = fixture("DictionaryBundle-plain.fixture");
  bytes[bytes.size() - 2] = 3;
  bytes.insert(bytes.end(), {'z', 'i', 'p'});
  const auto original = publish(bytes);
  HalDictionaryBindings bindings(cache, scratch);
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", {canonical, original}));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", {canonical, original}));
  HalDictionaryRemovalPlanAssembly assembly(cache, scratch);
  DictionaryRemovalPlan output;
  ASSERT_EQ(assembly.assemble("/dictionaries/es", request(original), output), DictionaryRemovalAssemblyResult::Match);
  EXPECT_EQ(output.installed.archives.original, original);
  EXPECT_EQ(output.installed.archives.members, canonical);
  const auto saved = output;
  const auto before = inventory_hal_test::state.files;
  EXPECT_EQ(assembly.assemble("/dictionaries/es", request(canonical), output),
            DictionaryRemovalAssemblyResult::Unrelated);
  EXPECT_EQ(output, saved);
  EXPECT_EQ(inventory_hal_test::state.files, before);
}
TEST_F(DictionaryRemovalPlanAssemblyTest, ChangedMembersAndReadCloseOrMissingCacheFailuresNeverPublishProofs) {
  for (unsigned fault = 0; fault < 4; ++fault) {
    const auto manifest = populate();
    auto& state = inventory_hal_test::state;
    if (fault == 0) state.files["/dictionaries/es/stem.dict"][0] ^= 1;
    if (fault == 1) state.readErrorPath = "/dictionaries/es/stem.dict";
    if (fault == 2) state.failClosePath = "/dictionaries/es/stem.dict";
    if (fault == 3) {
      for (auto it = state.files.begin(); it != state.files.end();) {
        if (it->first.starts_with("/.crosspoint/companion/dictionary-") &&
            !it->first.starts_with("/.crosspoint/companion/dictionary-binding-"))
          it = state.files.erase(it);
        else
          ++it;
      }
    }
    const auto before = state.files;
    DictionaryRemovalPlan output;
    output.request.owner.fill(99);
    const auto saved = output;
    HalDictionaryRemovalPlanAssembly assembly(cache, scratch);
    EXPECT_EQ(assembly.assemble("/dictionaries/es", request(manifest), output), DictionaryRemovalAssemblyResult::Error)
        << fault;
    EXPECT_EQ(output, saved);
    EXPECT_EQ(state.files, before);
  }
}
TEST_F(DictionaryRemovalPlanAssemblyTest, CancellationAtEveryGuardStopsWithoutChangingOutputOrFiles) {
  const auto manifest = populate();
  const auto before = inventory_hal_test::state.files;
  struct Guard {
    unsigned calls = 0, cancelAt = UINT32_MAX;
  } guard;
  HalDictionaryRemovalPlanAssembly assembly(
      cache, scratch,
      [](void* context) {
        auto& g = *static_cast<Guard*>(context);
        return ++g.calls != g.cancelAt;
      },
      &guard);
  DictionaryRemovalPlan output;
  ASSERT_EQ(assembly.assemble("/dictionaries/es", request(manifest), output), DictionaryRemovalAssemblyResult::Match);
  const auto checks = guard.calls;
  for (unsigned at = 1; at <= checks; ++at) {
    guard.calls = 0;
    guard.cancelAt = at;
    const auto saved = output;
    EXPECT_EQ(assembly.assemble("/dictionaries/es", request(manifest), output), DictionaryRemovalAssemblyResult::Error)
        << at;
    EXPECT_EQ(output, saved);
    EXPECT_EQ(inventory_hal_test::state.files, before);
  }
}
TEST_F(DictionaryRemovalPlanAssemblyTest, EmptyFoldersAndMalformedBindingsPreserveCallerProof) {
  const auto manifest = populate();
  auto& state = inventory_hal_test::state;
  state.directories["/dictionaries/empty"] = {};
  HalDictionaryRemovalPlanAssembly assembly(cache, scratch);
  DictionaryRemovalPlan output;
  output.request.owner.fill(99);
  const auto saved = output;
  EXPECT_EQ(assembly.assemble("/dictionaries/empty", request(manifest), output),
            DictionaryRemovalAssemblyResult::Empty);
  EXPECT_EQ(output, saved);
  HalDictionaryBindings bindings(cache, scratch);
  ASSERT_TRUE(bindings.install("/dictionaries/es/stem", {manifest, manifest}));
  ASSERT_TRUE(bindings.finalizeInstallation("/dictionaries/es/stem", {manifest, manifest}));
  for (auto& [path, bytes] : state.files)
    if (path.starts_with("/.crosspoint/companion/dictionary-binding-")) bytes[0] ^= 1;
  const auto before = state.files;
  EXPECT_EQ(assembly.assemble("/dictionaries/es", request(manifest), output), DictionaryRemovalAssemblyResult::Error);
  EXPECT_EQ(output, saved);
  EXPECT_EQ(state.files, before);
}
TEST_F(DictionaryRemovalPlanAssemblyTest, ShortOrOverlappingScratchRefusesBeforeReadingOrChangingOutput) {
  const auto manifest = populate();
  DictionaryRemovalPlan output;
  output.request.owner.fill(99);
  const auto saved = output;
  const auto files = inventory_hal_test::state.files;
  const auto opens = inventory_hal_test::state.opens;
  HalDictionaryRemovalPlanAssembly shortScratch(cache, std::span(scratch).first(64));
  EXPECT_EQ(shortScratch.assemble("/dictionaries/es", request(manifest), output),
            DictionaryRemovalAssemblyResult::Error);
  HalDictionaryRemovalPlanAssembly overlap(cache, {reinterpret_cast<uint8_t*>(&output), sizeof(output)});
  EXPECT_EQ(overlap.assemble("/dictionaries/es", request(manifest), output), DictionaryRemovalAssemblyResult::Error);
  EXPECT_EQ(output, saved);
  EXPECT_EQ(inventory_hal_test::state.files, files);
  EXPECT_EQ(inventory_hal_test::state.opens, opens);
}

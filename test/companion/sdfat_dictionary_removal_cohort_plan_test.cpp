#include <openssl/evp.h>

#include <cassert>

#include "lib/hal/HalDictionaryRemovalCohortPlanStorage.h"
using namespace companion;
namespace {
DictionaryRemovalPlan plan(bool compressed = false, bool synonyms = false) {
  DictionaryRemovalPlan result;
  result.request.transaction.fill(1);
  result.request.owner.fill(2);
  result.request.generation.fill(3);
  result.request.manifest.kind = ContentKind::Dictionary;
  result.request.manifest.formatVersion = 1;
  result.request.manifest.length = 1000;
  result.request.manifest.contentHash.fill(4);
  auto& installed = result.installed;
  installed.revision = 1;
  installed.phase = DictionaryInstallationPhase::Committed;
  installed.extraction.revision = 1;
  installed.extraction.transaction = result.request.transaction;
  installed.extraction.generation = result.request.generation;
  installed.extraction.archiveHash = result.request.manifest.contentHash;
  installed.extraction.compressed = compressed;
  installed.extraction.synonyms = synonyms;
  installed.extraction.sealed = installed.published = synonyms ? 15 : 7;
  installed.extraction.lengths = {6, 24, 111, synonyms ? 12U : 0U};
  for (unsigned at = 0; at < (synonyms ? 4U : 3U); ++at) installed.extraction.hashes[at].fill(at + 5);
  installed.archives.original = result.request.manifest;
  installed.archives.members = result.request.manifest;
  installed.archives.members.contentHash.fill(9);
  std::strcpy(installed.base.data(), "/dictionaries/Family/dictionary");
  return result;
}

std::string address(const Digest& digest) {
  std::string result = "/.crosspoint/companion/removal-dictionary-plan-";
  result.reserve(128);
  static constexpr char DIGITS[] = "0123456789abcdef";
  for (auto byte : digest) {
    result += DIGITS[byte >> 4];
    result += DIGITS[byte & 15];
  }
  return result;
}
}  // namespace
int main() {
  for (unsigned fault = 0; fault < 8; ++fault) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    state.directories["/"] = {};
    state.directories["/.crosspoint"] = {};
    state.directories["/.crosspoint/companion"] = {};
    auto first = plan(true, true), second = first;
    std::strcpy(first.installed.base.data(), "/.dictionaries/Family/dictionary");
    std::vector<uint8_t> bytes(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + 2 * DICTIONARY_REMOVAL_PLAN_SIZE);
    auto payload = std::span(bytes).subspan(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE);
    assert(DictionaryRemovalPlanCodec::encode(first, payload.first(DICTIONARY_REMOVAL_PLAN_SIZE)) ==
           DICTIONARY_REMOVAL_PLAN_SIZE);
    assert(DictionaryRemovalPlanCodec::encode(second, payload.subspan(DICTIONARY_REMOVAL_PLAN_SIZE)) ==
           DICTIONARY_REMOVAL_PLAN_SIZE);
    DictionaryRemovalCohortHeader header{first.request, 7, 2, inventoryIndexCrc(payload)};
    assert(encodeDictionaryRemovalCohortHeader(header, bytes) == DICTIONARY_REMOVAL_COHORT_HEADER_SIZE);
    Digest digest{};
    EVP_Digest(bytes.data(), bytes.size(), digest.data(), nullptr, EVP_sha256(), nullptr);
    const auto path = address(digest);
    state.files[path] = bytes;
    HalDictionaryRemovalCohortPlanStorage storage;
    if (fault == 1) state.files[path][DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + 20] ^= 1;
    if (fault == 2) state.readErrorPath = path;
    if (fault == 3) state.failSyncPath = path;
    if (fault == 4) state.files[path].push_back(0);
    if (fault >= 1 && fault <= 4) {
      assert(storage.open(digest, first.request) != DictionaryRemovalCohortStorageResult::Ok);
      assert(!storage.current());
      assert(!storage.verifiedDigest());
      state.readErrorPath.clear();
      state.failSyncPath.clear();
      state.files[path] = bytes;
    }
    assert(storage.open(digest, first.request) == DictionaryRemovalCohortStorageResult::Ok);
    assert(storage.current()->count == 2);
    assert(*storage.verifiedDigest() == digest);
    DictionaryRemovalPlan output;
    assert(storage.next(output) == InventoryPathRecordResult::Entry);
    assert(output == first);
    assert(storage.next(output) == InventoryPathRecordResult::Entry);
    assert(output == second);
    assert(storage.next(output) == InventoryPathRecordResult::End);
    assert(storage.rewind());
    if (fault == 5) {
      state.files[path][DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + 20] ^= 1;
      assert(!storage.rewind());
      assert(!storage.current());
      state.files[path] = bytes;
      assert(storage.open(digest, first.request) == DictionaryRemovalCohortStorageResult::Ok);
    }
    if (fault == 6) {
      const auto retained = output;
      state.readErrorPath = path;
      assert(storage.next(output) == InventoryPathRecordResult::Error);
      assert(output == retained);
      state.readErrorPath.clear();
      assert(storage.open(digest, first.request) == DictionaryRemovalCohortStorageResult::Ok);
    }
    if (fault == 7) {
      state.failClosePath = path;
      assert(!storage.close());
      assert(!storage.current());
      state.failClosePath.clear();
      assert(storage.open(digest, first.request) == DictionaryRemovalCohortStorageResult::Ok);
    }
    assert(storage.close());
    auto foreign = first.request;
    foreign.owner.fill(8);
    assert(storage.open(digest, foreign) == DictionaryRemovalCohortStorageResult::Corrupt);
    assert(!storage.current());
    Digest missing{};
    missing.fill(9);
    assert(storage.open(missing, first.request) == DictionaryRemovalCohortStorageResult::Missing);
    assert(storage.open(Digest{}, first.request) == DictionaryRemovalCohortStorageResult::Invalid);
    assert(state.files.at(path) == bytes);
  }
}

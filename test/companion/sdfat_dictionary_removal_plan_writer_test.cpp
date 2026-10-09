#include <cassert>

#include "lib/hal/HalDictionaryRemovalCohortPlanStorage.h"
#include "lib/hal/HalDictionaryRemovalCohortPlanWriter.h"
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

}  // namespace
int main() {
  const uint64_t quota = DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + 2 * DICTIONARY_REMOVAL_PLAN_SIZE;
  for (unsigned fault = 0; fault < 10; ++fault) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    state.directories["/"] = {};
    auto first = plan(true, true), second = first;
    std::strcpy(first.installed.base.data(), "/.dictionaries/Family/dictionary");
    Digest digest{};
    std::string target;
    {
      HalDictionaryRemovalCohortPlanWriter writer(fault == 8 ? quota - DICTIONARY_REMOVAL_PLAN_SIZE : quota);
      assert(writer.begin(first.request, 7));
      std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> claimBytes{};
      HalRemovalStageClaimStorage claim(claimBytes);
      assert(claim.load({first.request, 7}) == RemovalStageClaimResult::Ok);
      const std::string stage = claim.planStagePath();
      if (fault == 1) state.corruptWritePath = stage;
      if (fault == 2) {
        state.failWritePath = stage;
        state.matchingWrites = 0;
        state.failMatchingWrite = 1;
      }
      const bool appended = writer.append(first);
      if (fault == 2)
        assert(!appended);
      else
        assert(appended);
      bool ready = false;
      if (appended) {
        const bool appendedSecond = writer.append(second);
        if (fault == 1 || fault == 8)
          assert(!appendedSecond);
        else {
          assert(appendedSecond);
          ready = true;
        }
      }
      if (ready) {
        if (fault == 9) {
          auto changed = second;
          changed.installed.extraction.hashes[0][0] ^= 1;
          auto staged = std::span(state.files.at(stage));
          const auto payloadCrc = inventoryIndexCrc(staged.subspan(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE));
          assert(DictionaryRemovalPlanCodec::encode(
                     changed, staged.subspan(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + DICTIONARY_REMOVAL_PLAN_SIZE)) ==
                 DICTIONARY_REMOVAL_PLAN_SIZE);
          // Valid record CRCs retain the CRC residue; intended SHA must still reject the changed proof.
          assert(inventoryIndexCrc(staged.subspan(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE)) == payloadCrc);
        }
        if (fault == 3) state.failSyncPath = stage;
        if (fault == 4) state.failClosePath = stage;
        if (fault == 5) state.failRename = state.renames + 1;
        if (fault == 6) state.failRenameAfter = state.renames + 1;
        assert(writer.finish(2) == (fault == 0 || fault == 7));
        if (writer.publishedDigest()) {
          digest = *writer.publishedDigest();
          target = writer.publishedPath();
        }
      }
      state.corruptWritePath.clear();
      state.failWritePath.clear();
      state.failSyncPath.clear();
      state.failClosePath.clear();
      state.failRename = state.failRenameAfter = 0;
    }
    if (fault == 7) {
      const auto before = state.files.at(target);
      HalDictionaryRemovalCohortPlanWriter repeated(quota);
      assert(repeated.begin(first.request, 7));
      assert(repeated.append(first));
      assert(repeated.append(second));
      state.failRemoveAfter = true;
      assert(!repeated.finish(2));
      state.failRemoveAfter = false;
      assert(state.files.at(target) == before);
    }
    HalDictionaryRemovalCohortPlanWriter retry(quota);
    assert(retry.begin(first.request, 7));
    assert(retry.append(first));
    assert(retry.append(second));
    assert(retry.finish(2));
    if (!target.empty()) assert(target == retry.publishedPath());
    digest = *retry.publishedDigest();
    target = retry.publishedPath();
    HalDictionaryRemovalCohortPlanStorage reader;
    assert(reader.open(digest, first.request) == DictionaryRemovalCohortStorageResult::Ok);
    DictionaryRemovalPlan output;
    assert(reader.next(output) == InventoryPathRecordResult::Entry);
    assert(output == first);
    assert(reader.next(output) == InventoryPathRecordResult::Entry);
    assert(output == second);
    assert(reader.next(output) == InventoryPathRecordResult::End);
    assert(reader.close());
    const auto retained = state.files;
    assert(retry.discard());
    assert(state.files == retained);
  }
  for (unsigned variant = 0; variant < 3; ++variant) {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    state.directories["/"] = {};
    auto first = plan(), second = first;
    first.installed.base.fill(0);
    second.installed.base.fill(0);
    std::strcpy(first.installed.base.data(), "/dictionaries/A/stem");
    std::strcpy(second.installed.base.data(), "/dictionaries/a/stem");
    if (variant == 1) second = first;
    if (variant == 2) second.request.owner.fill(8);
    HalDictionaryRemovalCohortPlanWriter writer(quota);
    assert(validDictionaryRemovalPlan(first));
    assert(writer.begin(first.request, 7));
    assert(writer.append(first));
    const auto before = state.files;
    assert(!writer.append(second));
    assert(state.files == before);
    assert(writer.discard());
  }
  {
    auto& state = inventory_hal_test::state;
    state = {};
    state.enumerateFileMap = true;
    state.directories["/"] = {};
    const auto first = plan();
    HalDictionaryRemovalCohortPlanWriter writer(quota);
    assert(writer.begin(first.request, 7));
    std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> claimBytes{};
    HalRemovalStageClaimStorage claim(claimBytes);
    assert(claim.load({first.request, 7}) == RemovalStageClaimResult::Ok);
    const std::string stage = claim.planStagePath();
    auto foreign = first.request;
    foreign.owner.fill(9);
    DictionaryRemovalCohortHeader header{foreign, 7, 1, 0};
    assert(encodeDictionaryRemovalCohortHeader(header, state.files.at(stage)) == DICTIONARY_REMOVAL_COHORT_HEADER_SIZE);
    const auto before = state.files;
    assert(!writer.discard());
    assert(state.files == before);
    HalDictionaryRemovalCohortPlanWriter retry(quota);
    assert(!retry.begin(first.request, 7));
    assert(state.files == before);
  }
}

#pragma once

#include "CompanionDictionaryInstallationParent.h"

namespace companion {
inline constexpr size_t DICTIONARY_RETIREMENT_PROOF_SIZE = 8 + DICTIONARY_INSTALLATION_PLAN_SIZE + 4;
inline bool dictionaryRetirementProofFromParent(const DictionaryInstallationParent& parent,
                                                DictionaryInstallationPlan& output) {
  const auto plan = parent.current();
  if (!plan || !parent.isPhase(TransferPhase::Committed) || plan->phase != DictionaryInstallationPhase::Committed)
    return false;
  output = *plan;
  return true;
}
// Independent of extraction/installation journals, which retirement removes.
inline bool dictionaryRetirementProofMatchesTransfer(const DictionaryInstallationPlan& proof,
                                                     const Transfer& transfer) {
  const auto state = transfer.current();
  const auto manifest = transfer.contentManifest();
  return state && manifest && validDictionaryInstallationPlan(proof) &&
         proof.phase == DictionaryInstallationPhase::Committed && state->phase == TransferPhase::Committed &&
         inventory_detail::nonzero(state->owner) && state->transaction == proof.extraction.transaction &&
         state->storageGeneration == proof.extraction.generation &&
         state->contentHash == proof.extraction.archiveHash && state->durableOffset == state->length &&
         *manifest == proof.archives.original && matchesTransferManifest(*manifest, *state) &&
         transfer.destination() == std::string_view(proof.base.data());
}
// Session-owned nested codec retains its working plan; buffers must be disjoint
// from the caller's plan. Persistence/readback must precede any journal removal.
class DictionaryRetirementProofCodec final {
 public:
  static size_t encode(const DictionaryInstallationPlan& plan, std::span<uint8_t> output) {
    if (output.size() < DICTIONARY_RETIREMENT_PROOF_SIZE || !validDictionaryInstallationPlan(plan) ||
        plan.phase != DictionaryInstallationPhase::Committed)
      return 0;
    auto bytes = output.first(DICTIONARY_RETIREMENT_PROOF_SIZE);
    std::fill(bytes.begin(), bytes.end(), 0);
    static constexpr std::array<uint8_t, 5> PREFIX = {'D', 'R', 'E', 'T', 1};
    std::copy(PREFIX.begin(), PREFIX.end(), bytes.begin());
    DictionaryInstallationPlanCodec::encode(plan, bytes.subspan(8, DICTIONARY_INSTALLATION_PLAN_SIZE));
    inventory_detail::write(bytes, bytes.size() - 4, inventoryIndexCrc(bytes.first(bytes.size() - 4)), 4);
    return bytes.size();
  }
  const DictionaryInstallationPlan* inspect(std::span<const uint8_t> bytes) {
    static constexpr std::array<uint8_t, 5> PREFIX = {'D', 'R', 'E', 'T', 1};
    if (bytes.size() != DICTIONARY_RETIREMENT_PROOF_SIZE || !std::equal(PREFIX.begin(), PREFIX.end(), bytes.begin()) ||
        bytes[5] || bytes[6] || bytes[7] ||
        inventory_detail::read(bytes, bytes.size() - 4, 4) != inventoryIndexCrc(bytes.first(bytes.size() - 4)))
      return nullptr;
    const auto plan = codec.inspect(bytes.subspan(8, DICTIONARY_INSTALLATION_PLAN_SIZE));
    return plan && plan->phase == DictionaryInstallationPhase::Committed ? plan : nullptr;
  }
  bool decode(std::span<const uint8_t> bytes, DictionaryInstallationPlan& output) {
    const auto plan = inspect(bytes);
    if (!plan) return false;
    output = *plan;
    return true;
  }

 private:
  DictionaryInstallationPlanCodec codec;
};
}  // namespace companion

#pragma once

#include "CompanionContentRemovalRequest.h"
#include "CompanionDictionaryInstallationPaths.h"

namespace companion {
// Retain the request and member proofs off stack for the serialized removal owner.
struct DictionaryRemovalPlan {
  ContentRemovalRequest request;
  DictionaryInstallationPlan installed;
  bool operator==(const DictionaryRemovalPlan&) const = default;
};
inline bool validDictionaryRemovalPlan(const DictionaryRemovalPlan& plan) {
  return validContentRemovalRequest(plan.request) && plan.request.manifest.kind == ContentKind::Dictionary &&
         validDictionaryInstallationPlan(plan.installed) &&
         plan.installed.phase == DictionaryInstallationPhase::Committed &&
         plan.installed.archives.original == plan.request.manifest &&
         plan.installed.extraction.transaction == plan.request.transaction &&
         plan.installed.extraction.generation == plan.request.generation;
}
inline constexpr size_t DICTIONARY_REMOVAL_INSTALLED_OFFSET = 8 + CONTENT_REMOVAL_REQUEST_SIZE;
inline constexpr size_t DICTIONARY_REMOVAL_PLAN_SIZE =
    DICTIONARY_REMOVAL_INSTALLED_OFFSET + DICTIONARY_INSTALLATION_PLAN_SIZE + 4;
class DictionaryRemovalPlanCodec final {
 public:
  static size_t encode(const DictionaryRemovalPlan& plan, std::span<uint8_t> output) {
    if (output.size() < DICTIONARY_REMOVAL_PLAN_SIZE || !validDictionaryRemovalPlan(plan)) return 0;
    auto bytes = output.first(DICTIONARY_REMOVAL_PLAN_SIZE);
    static constexpr std::array<uint8_t, 8> PREFIX = {'D', 'R', 'E', 'M', 1, 0, 0, 0};
    std::copy(PREFIX.begin(), PREFIX.end(), bytes.begin());
    if (encodeContentRemovalRequest(plan.request, bytes.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE)) !=
            CONTENT_REMOVAL_REQUEST_SIZE ||
        DictionaryInstallationPlanCodec::encode(
            plan.installed, bytes.subspan(DICTIONARY_REMOVAL_INSTALLED_OFFSET, DICTIONARY_INSTALLATION_PLAN_SIZE)) !=
            DICTIONARY_INSTALLATION_PLAN_SIZE)
      return 0;
    inventory_detail::write(bytes, DICTIONARY_REMOVAL_PLAN_SIZE - 4,
                            inventoryIndexCrc(bytes.first(DICTIONARY_REMOVAL_PLAN_SIZE - 4)), 4);
    return DICTIONARY_REMOVAL_PLAN_SIZE;
  }
  bool decode(std::span<const uint8_t> bytes, DictionaryRemovalPlan& output) {
    static constexpr std::array<uint8_t, 8> PREFIX = {'D', 'R', 'E', 'M', 1, 0, 0, 0};
    if (bytes.size() != DICTIONARY_REMOVAL_PLAN_SIZE || !std::equal(PREFIX.begin(), PREFIX.end(), bytes.begin()) ||
        inventoryIndexCrc(bytes.first(DICTIONARY_REMOVAL_PLAN_SIZE - 4)) !=
            inventory_detail::read(bytes, DICTIONARY_REMOVAL_PLAN_SIZE - 4, 4) ||
        !decodeContentRemovalRequest(bytes.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE), candidate.request) ||
        !installation.decode(bytes.subspan(DICTIONARY_REMOVAL_INSTALLED_OFFSET, DICTIONARY_INSTALLATION_PLAN_SIZE),
                             candidate.installed) ||
        !validDictionaryRemovalPlan(candidate))
      return false;
    output = candidate;
    return true;
  }

 private:
  DictionaryInstallationPlanCodec installation;
  DictionaryRemovalPlan candidate;
};
inline bool dictionaryRemovalMemberPath(const DictionaryRemovalPlan& plan, unsigned member, std::span<char> output) {
  return validDictionaryRemovalPlan(plan) && dictionaryInstallationMemberPath(plan.installed, member, output);
}
}  // namespace companion

#pragma once

#include "HalDictionaryDestinationLookup.h"
#include "HalDictionaryMemberVerification.h"

namespace companion {
// Serialized owner proves durable reservation, parent authorization and full
// ancestor/FAT name equivalence. The guard must remain valid through each call.
class HalDictionaryMemberPublicationStorage final : public DictionaryMemberPublicationStorage {
 public:
  using OwnerGuard = bool (*)(void*, const DictionaryInstallationPlan&);
  HalDictionaryMemberPublicationStorage(HalDictionaryDestinationLookup& destination,
                                        HalDictionaryMemberVerification& verification, OwnerGuard guard, void* owner,
                                        InventoryHashProgress progress = nullptr, void* context = nullptr)
      : destination(destination), verification(verification), guard(guard), owner(owner), stages(progress, context) {}
  DictionaryMemberPresence inspect(const DictionaryInstallationPlan& plan, unsigned member, bool installed) override {
    if (!authorized(plan) || member >= 4 || (member == 3 && !plan.extraction.synonyms)) return error("ownership");
    const auto ancestors = destination.inspectAncestors(plan);
    if (!authorized(plan)) return error("changed owner");
    if (ancestors == DictionaryDestinationPresence::Conflict) return DictionaryMemberPresence::Conflict;
    if (ancestors != DictionaryDestinationPresence::Present) return error("destination ancestors");
    if (installed) {
      const auto status = destination.inspect(plan, member);
      if (!authorized(plan)) return error("changed owner");
      if (status == DictionaryDestinationPresence::Missing) return DictionaryMemberPresence::Missing;
      if (status == DictionaryDestinationPresence::Conflict) return DictionaryMemberPresence::Conflict;
      if (status == DictionaryDestinationPresence::Error) return error("destination lookup");
    } else {
      const auto status = stages.inspect(HalZipEntryStage::memberPath(static_cast<HalZipEntryStage::Member>(member)));
      if (!authorized(plan)) return error("changed owner");
      if (status == CompanionFilePresence::Missing) return DictionaryMemberPresence::Missing;
      if (status == CompanionFilePresence::Error) return error("stage lookup");
    }
    const auto result = verification.verify(plan, member, installed);
    return authorized(plan) ? result : error("changed owner");
  }
  bool move(const DictionaryInstallationPlan& plan, unsigned member) override {
    if (plan.phase != DictionaryInstallationPhase::Publishing || member >= 4 ||
        inspect(plan, member, false) != DictionaryMemberPresence::Verified ||
        inspect(plan, member, true) != DictionaryMemberPresence::Missing ||
        !dictionaryInstallationMemberPath(plan, member, path) || !authorized(plan)) {
      error("move preconditions");
      return false;
    }
    if (!Storage.rename(HalZipEntryStage::memberPath(static_cast<HalZipEntryStage::Member>(member)), path.data())) {
      error("rename");
      return false;
    }
    if (!authorized(plan)) {
      error("changed owner after rename");
      return false;
    }
    return true;
  }

 private:
  HalDictionaryDestinationLookup& destination;
  HalDictionaryMemberVerification& verification;
  OwnerGuard guard;
  void* owner;
  HalCompanionFileLookup stages;
  std::array<char, DICTIONARY_INSTALLATION_MEMBER_PATH_CAPACITY> path{};
  bool authorized(const DictionaryInstallationPlan& plan) const {
    return guard && validDictionaryInstallationPlan(plan) && guard(owner, plan);
  }
  static DictionaryMemberPresence error(const char* reason) {
    LOG_ERR("COMPANION", "Dictionary member publication %s failed", reason);
    return DictionaryMemberPresence::Error;
  }
};
}  // namespace companion

#pragma once

#include "HalDictionaryBindings.h"
#include "HalDictionaryMemberPublicationStorage.h"

namespace companion {
// Borrowed providers and serialized owner guard; verification never mutates SD.
class HalDictionaryInstalledVerification final {
 public:
  using OwnerGuard = HalDictionaryMemberPublicationStorage::OwnerGuard;
  HalDictionaryInstalledVerification(DictionaryMemberPublicationStorage& members, HalDictionaryBindings& bindings,
                                     OwnerGuard guard, void* owner)
      : members(members), bindings(bindings), guard(guard), owner(owner) {}
  bool verify(const DictionaryInstallationPlan& proof) {
    if (!authorized(proof)) return failure("ownership");
    for (unsigned member = 0; member < (proof.extraction.synonyms ? 4u : 3u); ++member) {
      if (members.inspect(proof, member, false) != DictionaryMemberPresence::Missing || !authorized(proof) ||
          members.inspect(proof, member, true) != DictionaryMemberPresence::Verified || !authorized(proof))
        return failure("members");
    }
    if (!bindings.verifyFinalized(proof.base.data(), proof.archives) || !authorized(proof))
      return failure("binding or archives");
    return true;
  }

 private:
  DictionaryMemberPublicationStorage& members;
  HalDictionaryBindings& bindings;
  OwnerGuard guard;
  void* owner;
  bool authorized(const DictionaryInstallationPlan& proof) const {
    return guard && validDictionaryInstallationPlan(proof) && proof.phase == DictionaryInstallationPhase::Committed &&
           guard(owner, proof);
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Dictionary installed verification %s failed", reason);
    return false;
  }
};
}  // namespace companion

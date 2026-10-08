#pragma once

#include "HalDictionaryCacheStorage.h"
#include "HalDictionaryInstalledVerification.h"
#include "HalDictionaryPlanBindingStorage.h"

namespace companion {
// Session-owned outside the task stack. Parent, lookup, verification and scratch
// remain borrowed under exclusive installation ownership throughout recovery.
class HalDictionaryPublicationSession final {
 public:
  HalDictionaryPublicationSession(DictionaryInstallationParent& parent, HalDictionaryDestinationLookup& lookup,
                                  HalDictionaryMemberVerification& verification, std::span<uint8_t> scratch,
                                  InventoryHashProgress progress = nullptr, void* context = nullptr)
      : parent(parent),
        cache(progress, context),
        records(cache, scratch, progress, context),
        members(lookup, verification, guard, this, progress, context),
        publication(parent, members),
        bindingStorage(records),
        binding(parent, members, bindingStorage),
        installed(members, records, guard, this) {}
  DictionaryJournalResult install() {
    const auto recovered = parent.recover();
    if (recovered != DictionaryJournalResult::Ok) return fail(recovered, "plan recovery");
    const auto plan = parent.current();
    if (!plan || !parent.isPhase(TransferPhase::Installing))
      return fail(DictionaryJournalResult::Conflict, "installation phase");
    if (plan->phase == DictionaryInstallationPhase::Prepared ||
        plan->phase == DictionaryInstallationPhase::Publishing) {
      const auto result = publication.publish();
      if (result != DictionaryJournalResult::Ok) return fail(result, "member publication");
    }
    const auto result = binding.install();
    return result == DictionaryJournalResult::Ok ? result : fail(result, "binding installation");
  }
  DictionaryJournalResult finalize() {
    const auto recovered = parent.recover();
    if (recovered != DictionaryJournalResult::Ok) return fail(recovered, "final plan recovery");
    const auto result = binding.finalize();
    return result == DictionaryJournalResult::Ok ? result : fail(result, "binding finalization");
  }
  bool verifyInstalled() {
    const auto result = parent.recover();
    if (result != DictionaryJournalResult::Ok) {
      fail(result, "installed plan recovery");
      return false;
    }
    const auto plan = parent.current();
    if (!plan || !parent.isPhase(TransferPhase::Committed) || !installed.verify(*plan)) {
      fail(DictionaryJournalResult::Conflict, "installed verification");
      return false;
    }
    return true;
  }

 private:
  DictionaryInstallationParent& parent;
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings records;
  HalDictionaryMemberPublicationStorage members;
  DictionaryMemberPublication publication;
  HalDictionaryPlanBindingStorage bindingStorage;
  DictionaryPlanBinding binding;
  HalDictionaryInstalledVerification installed;
  static bool guard(void* context, const DictionaryInstallationPlan& expected) {
    const auto& session = *static_cast<HalDictionaryPublicationSession*>(context);
    const auto plan = session.parent.current();
    return plan && *plan == expected &&
           (session.parent.isPhase(TransferPhase::Installing) || session.parent.isPhase(TransferPhase::Committed));
  }
  static DictionaryJournalResult fail(DictionaryJournalResult result, const char* operation) {
    LOG_ERR("COMPANION", "Dictionary publication session %s failed: %u", operation, static_cast<unsigned>(result));
    return result;
  }
};
}  // namespace companion

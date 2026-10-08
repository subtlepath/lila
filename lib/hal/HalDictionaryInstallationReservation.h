#pragma once

#include "HalDictionaryDestinationLookup.h"
#include "HalDictionaryMemberVerification.h"

namespace companion {
// Serialized owner supplies a canonical-proof plan and keeps the destination
// exclusive through recovery/publication. Creates only unambiguous directories.
class HalDictionaryInstallationReservation final {
 public:
  HalDictionaryInstallationReservation(DictionaryInstallationParent& parent, HalDictionaryDestinationLookup& lookup,
                                       HalDictionaryMemberVerification& verification)
      : parent(parent), lookup(lookup), verification(verification) {}
  DictionaryJournalResult begin(const DictionaryInstallationPlan& plan) {
    if (!validDictionaryInstallationPlan(plan) || plan.revision != 1 ||
        plan.phase != DictionaryInstallationPhase::Prepared || parent.current() || !parent.canBegin(plan))
      return fail(DictionaryJournalResult::Conflict, "initial plan");
    const auto checked = verify(plan);
    if (checked != DictionaryJournalResult::Ok) return checked;
    const auto result = parent.begin(plan);
    return result == DictionaryJournalResult::Ok ? result : fail(result, "durable plan");
  }
  DictionaryJournalResult reserve(const DictionaryInstallationPlan& plan) {
    if (!validDictionaryInstallationPlan(plan) || plan.revision != 1 ||
        plan.phase != DictionaryInstallationPhase::Prepared || !parent.canBegin(plan))
      return fail(DictionaryJournalResult::Conflict, "reservation ownership");
    const auto result = parent.recover(plan);
    if (result == DictionaryJournalResult::Missing) return begin(plan);
    if (result != DictionaryJournalResult::Ok) return fail(result, "reservation recovery");
    const auto current = parent.current();
    if (!current || current->phase != DictionaryInstallationPhase::Prepared)
      return fail(DictionaryJournalResult::Conflict, "publication already started");
    return verify(*current);
  }

 private:
  DictionaryJournalResult verify(const DictionaryInstallationPlan& plan) {
    for (unsigned at = 0; at < (plan.extraction.synonyms ? 4u : 3u); ++at) {
      const auto result = verification.verify(plan, at, false);
      if (result != DictionaryMemberPresence::Verified)
        return fail(result == DictionaryMemberPresence::Error ? DictionaryJournalResult::IoError
                                                              : DictionaryJournalResult::Conflict,
                    "staged receipt");
    }
    const auto folder = lookup.prepareEmptyFolder(plan);
    if (folder != DictionaryDestinationFolder::Empty)
      return fail(folder == DictionaryDestinationFolder::Error ? DictionaryJournalResult::IoError
                                                               : DictionaryJournalResult::Conflict,
                  "destination not verified empty");
    return DictionaryJournalResult::Ok;
  }
  DictionaryInstallationParent& parent;
  HalDictionaryDestinationLookup& lookup;
  HalDictionaryMemberVerification& verification;
  static DictionaryJournalResult fail(DictionaryJournalResult result, const char* reason) {
    LOG_ERR("COMPANION", "Dictionary destination reservation %s failed", reason);
    return result;
  }
};
}  // namespace companion

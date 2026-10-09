#pragma once

#include "HalDictionaryRemovalStorage.h"

namespace companion {
// Retain outside the task stack. One fixed member owner/plan is reused; the
// parent excludes plan and namespace writers throughout each serialized walk.
class HalDictionaryRemovalCohortParticipant final : public ContentRemovalParticipant {
 public:
  HalDictionaryRemovalCohortParticipant(ContentRemovalJournal& journal, HalDictionaryRemovalCohortPlanStorage& plans,
                                        DictionaryRemovalReferences& references, std::span<uint8_t> scratch)
      : journal(journal), plans(plans), members(journal, {}, scratch), worker(journal, members, references) {}
  bool closeReaders() { return release(); }
  bool verifyPlan(const ContentRemovalRecord& r) override { return walk(r, &DictionaryRemovalParticipant::verifyPlan); }
  bool quarantine(const ContentRemovalRecord& r) override { return walk(r, &DictionaryRemovalParticipant::quarantine); }
  bool verifyQuarantined(const ContentRemovalRecord& r) override {
    return walk(r, &DictionaryRemovalParticipant::verifyQuarantined);
  }
  bool publishRemoval(const ContentRemovalRecord& r) override {
    return walk(r, &DictionaryRemovalParticipant::publishRemoval);
  }
  bool verifyPublished(const ContentRemovalRecord& r) override {
    return walk(r, &DictionaryRemovalParticipant::verifyPublished);
  }
  bool retireBackups(const ContentRemovalRecord& r) override {
    return r.phase == ContentRemovalPhase::Committed && walk(r, &DictionaryRemovalParticipant::verifyPublished) &&
           walk(r, &DictionaryRemovalParticipant::retireReferences) &&
           walk(r, &DictionaryRemovalParticipant::verifyPublished) &&
           walk(r, &DictionaryRemovalParticipant::retireVerifiedBackups);
  }
  bool verifyRetired(const ContentRemovalRecord& r) override {
    return walk(r, &DictionaryRemovalParticipant::verifyRetired);
  }

 private:
  ContentRemovalJournal& journal;
  HalDictionaryRemovalCohortPlanStorage& plans;
  HalDictionaryRemovalStorage members;
  DictionaryRemovalParticipant worker;
  DictionaryRemovalPlan plan;
  ContentRemovalRecord checkpoint;
  bool preflight = false;
  bool guard() const { return preflight ? !journal.current() : journal.current() && *journal.current() == checkpoint; }
  bool release() {
    worker.unbind();
    return members.unbind();
  }
  bool walk(const ContentRemovalRecord& r,
            bool (DictionaryRemovalParticipant::*operation)(const ContentRemovalRecord&)) {
    if (!validContentRemovalRecord(r) || !plans.current() || !plans.verifiedDigest() ||
        plans.current()->request != r.request || *plans.verifiedDigest() != r.planHash)
      return fail();
    checkpoint = r;
    preflight = !journal.current();
    if (preflight &&
        (r.phase != ContentRemovalPhase::Prepared || operation != &DictionaryRemovalParticipant::verifyPlan))
      return fail();
    if (!guard() || !plans.rewind() || !guard()) return fail();
    const uint64_t count = plans.current()->count;
    for (uint64_t ordinal = 0; ordinal < count; ++ordinal) {
      if (!guard() || !release() || !plans.rewind() || !guard()) return fail();
      for (uint64_t at = 0; at <= ordinal; ++at)
        if (plans.next(plan) != InventoryPathRecordResult::Entry || !guard()) return fail();
      if (!members.configureCohort(plans, ordinal) || !worker.bind(plan, r.planHash) || !guard() ||
          !(worker.*operation)(r) || !guard()) {
        release();
        return fail();
      }
    }
    return release() && guard() && plans.rewind() && guard();
  }
  static bool fail() {
    LOG_ERR("COMPANION", "Dictionary removal cohort operation failed");
    return false;
  }
};
}  // namespace companion

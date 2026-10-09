#pragma once

#include "HalSingleFileRemovalParticipant.h"

namespace companion {
// Off-stack serialized owner; plans, journal, references and scratch outlive it.
class HalSingleFileRemovalCohortParticipant final : public ContentRemovalParticipant {
 public:
  HalSingleFileRemovalCohortParticipant(ContentRemovalJournal& journal, HalMultiPathRemovalPlanStorage& plans,
                                        SingleFileRemovalReferences& references, std::span<uint8_t> scratch)
      : journal(journal), plans(plans), references(references), worker(journal, references, scratch) {}
  bool verifyPlan(const ContentRemovalRecord& r) override {
    return walk(r, &HalSingleFileRemovalParticipant::verifyPlan);
  }
  bool quarantine(const ContentRemovalRecord& r) override {
    return walk(r, &HalSingleFileRemovalParticipant::quarantine);
  }
  bool verifyQuarantined(const ContentRemovalRecord& r) override {
    return walk(r, &HalSingleFileRemovalParticipant::verifyQuarantined);
  }
  bool publishRemoval(const ContentRemovalRecord& r) override {
    return walk(r, &HalSingleFileRemovalParticipant::publishRemoval);
  }
  bool verifyPublished(const ContentRemovalRecord& r) override {
    return walk(r, &HalSingleFileRemovalParticipant::verifyPublished);
  }
  bool retireBackups(const ContentRemovalRecord& r) override {
    return walk(r, &HalSingleFileRemovalParticipant::retireBackups);
  }
  bool verifyRetired(const ContentRemovalRecord& r) override {
    return walk(r, &HalSingleFileRemovalParticipant::verifyRetired);
  }

 private:
  ContentRemovalJournal& journal;
  HalMultiPathRemovalPlanStorage& plans;
  SingleFileRemovalReferences& references;
  HalSingleFileRemovalParticipant worker;
  ContentRemovalRecord checkpoint;
  bool preflight = false;
  bool guard() const { return preflight ? !journal.current() : journal.current() && *journal.current() == checkpoint; }
  bool walk(const ContentRemovalRecord& r,
            bool (HalSingleFileRemovalParticipant::*operation)(const ContentRemovalRecord&)) {
    if (!validContentRemovalRecord(r) || !plans.current() || !plans.verifiedDigest() ||
        plans.current()->request != r.request || *plans.verifiedDigest() != r.planHash)
      return failure();
    checkpoint = r;
    preflight = !journal.current();
    if (preflight &&
        (r.phase != ContentRemovalPhase::Prepared || operation != &HalSingleFileRemovalParticipant::verifyPlan))
      return failure();
    if (!guard() || !plans.rewind()) return failure();
    const uint64_t count = plans.current()->count;
    for (uint64_t ordinal = 0; ordinal < count; ++ordinal) {
      if (!guard() || !worker.unbind() || !worker.bindCohortPath(plans, r.planHash, ordinal) || !guard() ||
          !(worker.*operation)(r) || !guard()) {
        worker.unbind();
        return failure();
      }
    }
    return worker.unbind() && guard() && plans.rewind() && guard();
  }
  static bool failure() {
    LOG_ERR("COMPANION", "Single-file removal cohort operation failed");
    return false;
  }
};
}  // namespace companion

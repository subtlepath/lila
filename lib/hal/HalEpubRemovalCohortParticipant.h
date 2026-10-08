#pragma once

#include "HalEpubRemovalReferences.h"
#include "HalRemovalPlanPathMatcher.h"

namespace companion {
// Off-stack serialized owner; plans, journal, references and scratch outlive it.
class HalEpubRemovalCohortParticipant final : public ContentRemovalParticipant {
 public:
  HalEpubRemovalCohortParticipant(ContentRemovalJournal& journal, HalMultiPathRemovalPlanStorage& plans,
                                  HalEpubRemovalReferences& references, std::span<uint8_t> scratch)
      : journal(journal),
        plans(plans),
        references(references),
        bridge(*this),
        matcher(plans, check, this),
        worker(journal, bridge, scratch) {}
  bool verifyPlan(const ContentRemovalRecord& r) override { return walk(r, &HalEpubRemovalParticipant::verifyPlan); }
  bool quarantine(const ContentRemovalRecord& r) override { return walk(r, &HalEpubRemovalParticipant::quarantine); }
  bool verifyQuarantined(const ContentRemovalRecord& r) override {
    return walk(r, &HalEpubRemovalParticipant::verifyQuarantined);
  }
  bool publishRemoval(const ContentRemovalRecord& r) override {
    return walk(r, &HalEpubRemovalParticipant::publishRemoval);
  }
  bool verifyPublished(const ContentRemovalRecord& r) override {
    return walk(r, &HalEpubRemovalParticipant::verifyPublished);
  }
  bool retireBackups(const ContentRemovalRecord& r) override {
    return walk(r, &HalEpubRemovalParticipant::retireBackups);
  }
  bool verifyRetired(const ContentRemovalRecord& r) override {
    return walk(r, &HalEpubRemovalParticipant::verifyRetired);
  }

 private:
  class Bridge final : public EpubRemovalReferences {
   public:
    explicit Bridge(HalEpubRemovalCohortParticipant& owner) : owner(owner) {}
    bool publish(const ContentRemovalRecord& r, const char*) override {
      return owner.guard() && owner.references.publishMatching(r, HalRemovalPlanPathMatcher::callback, &owner.matcher);
    }
    bool verify(const ContentRemovalRecord& r, const char* path) override {
      return owner.guard() && owner.references.verify(r, path);
    }
    bool retire(const ContentRemovalRecord& r, const char* path) override {
      return owner.guard() && owner.references.retire(r, path);
    }
    bool verifyRetired(const ContentRemovalRecord& r, const char* path) override {
      return owner.guard() && owner.references.verifyRetired(r, path);
    }

   private:
    HalEpubRemovalCohortParticipant& owner;
  };
  ContentRemovalJournal& journal;
  HalMultiPathRemovalPlanStorage& plans;
  HalEpubRemovalReferences& references;
  Bridge bridge;
  HalRemovalPlanPathMatcher matcher;
  HalEpubRemovalParticipant worker;
  ContentRemovalRecord checkpoint;
  bool preflight = false;
  static bool check(void* opaque) { return static_cast<HalEpubRemovalCohortParticipant*>(opaque)->guard(); }
  bool guard() const { return preflight ? !journal.current() : journal.current() && *journal.current() == checkpoint; }
  bool walk(const ContentRemovalRecord& r, bool (HalEpubRemovalParticipant::*operation)(const ContentRemovalRecord&)) {
    if (!validContentRemovalRecord(r) || !plans.current() || !plans.verifiedDigest() ||
        plans.current()->request != r.request || *plans.verifiedDigest() != r.planHash)
      return failure();
    checkpoint = r;
    preflight = !journal.current();
    if (preflight && (r.phase != ContentRemovalPhase::Prepared || operation != &HalEpubRemovalParticipant::verifyPlan))
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
    LOG_ERR("COMPANION", "EPUB removal cohort operation failed");
    return false;
  }
};
}  // namespace companion

#pragma once

#include "HalCompletedContentRemovals.h"
#include "HalContentRemovalJournalStorage.h"
#include "HalCourseRemovalBaseline.h"
#include "HalCourseRemovalMetadata.h"
#include "HalCourseRemovalPlanStorage.h"
#include "HalCourseRemovalProofStorage.h"
#include "HalCourseStateIsolation.h"

namespace companion {
// Retain off stack while all namespace/state writers remain excluded. The loan
// ends on close, another open, permission loss or destruction; no writes occur.
class HalRemovedCourseBaseline final {
 public:
  using Permission = bool (*)(void*);
  HalRemovedCourseBaseline(const Identity& generation, std::span<uint8_t> io, Permission permitted, void* context)
      : generation(generation),
        permitted(permitted),
        context(context),
        metadata(permitted, context),
        journal(journalStorage, journalBytes),
        proofs(proofBytes, permitted, context),
        completions(completionBytes),
        plans(comparisonBytes),
        isolation(metadata, io, permitted, context),
        baseline(journal, io, permitted, context),
        io(io) {}
  bool open(const ContentManifest& candidate) {
    ready = false;
    if (!closeReaders() || !guard() || generation == Identity{} || io.size() < COURSE_BINDING_SIZE ||
        !validCourseBinding(candidate) || candidate.formatVersion != 1 || candidate.contentHash == Digest{})
      return fail("admission");
    bool present = false;
    if (readCourseBinding(metadata, COURSE_BINDING_PATH, io, binding, present) != CourseBindingResult::Ok || !guard() ||
        !present || binding.logicalIdentity != candidate.logicalIdentity || binding.formatVersion != 1)
      return fail("retained binding");
    uint64_t length = 0;
    for (const auto* path : {COURSE_BINDING_STAGE, COURSE_BINDING_BACKUP, COURSE_REMOVAL_PROOF_STAGE})
      if (!guard() || metadata.stat(path, length) != FileStatus::Missing || !guard())
        return fail("unfinished publication");
    if (proofs.load(generation, proof) != CourseRemovalProofStorageResult::Ok || !guard() ||
        completions.load(proof.request, completed) != CompletedRemovalResult::Ok || !guard() ||
        !completedCourseRemovalProof(proof, completed, binding, generation) ||
        plans.load(proof.planHash, planBytes, plan) != CourseRemovalPlanStorageResult::Ok || !guard() ||
        plan.request != proof.request || !isolation.verify(binding.logicalIdentity) || !guard() ||
        !baseline.verifyCompleted(proof, completed, binding, generation) || !guard())
      return fail("completed baseline proof");
    ready = true;
    return true;
  }
  const char* path() const {
    if (!ready) return nullptr;
    if (!guard()) {
      ready = false;
      return nullptr;
    }
    const auto* result = baseline.path();
    if (!result) ready = false;
    return result;
  }
  const ContentManifest* manifest() const { return path() ? &binding : nullptr; }
  bool closeReaders() {
    ready = false;
    const bool metadataClosed = metadata.closeReaders();
    const bool proofClosed = proofs.closeReaders();
    const bool completionClosed = completions.closeReaders();
    const bool planClosed = plans.closeReaders();
    const bool stateClosed = isolation.closeReaders();
    const bool baselineClosed = baseline.closeReaders();
    return metadataClosed && proofClosed && completionClosed && planClosed && stateClosed && baselineClosed;
  }

 private:
  Identity generation;
  Permission permitted;
  void* context;
  HalCourseRemovalMetadata metadata;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{}, proofBytes{}, completionBytes{};
  std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> comparisonBytes{};
  std::array<uint8_t, COURSE_REMOVAL_PLAN_SIZE> planBytes{};
  HalContentRemovalJournalStorage journalStorage;
  ContentRemovalJournal journal;
  HalCourseRemovalProofStorage proofs;
  HalCompletedContentRemovals completions;
  HalCourseRemovalPlanStorage plans;
  HalCourseStateIsolation isolation;
  HalCourseRemovalBaseline baseline;
  std::span<uint8_t> io;
  ContentManifest binding;
  ContentRemovalRecord proof, completed;
  CourseRemovalPlan plan;
  mutable bool ready = false;
  bool guard() const { return permitted && permitted(context); }
  bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Removed course baseline %s failed", operation);
    closeReaders();
    return false;
  }
};
}  // namespace companion

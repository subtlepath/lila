#pragma once

#include "CompanionCourseRemovalParticipant.h"
#include "HalCourseRemovalBaseline.h"
#include "HalCourseRemovalProofStorage.h"
#include "HalCourseStateIsolation.h"

namespace companion {
// Retain off stack with exclusive metadata/state ownership. Scratch and stores
// are borrowed and disjoint from the sealed plan and journal/proof encoding.
class HalCourseRemovalReferences final : public CourseRemovalReferences {
 public:
  using Permission = bool (*)(void*);
  HalCourseRemovalReferences(ContentRemovalJournal& journal, TransferStorage& metadata, std::span<uint8_t> scratch,
                             HalCourseStateIsolation& isolation, HalCourseRemovalProofStorage& proofs,
                             HalCourseRemovalBaseline& baseline, Permission permitted, void* context)
      : journal(journal),
        metadata(metadata),
        scratch(scratch),
        isolation(isolation),
        proofs(proofs),
        baseline(baseline),
        permitted(permitted),
        context(context) {}
  bool verifyPlan(const ContentRemovalRecord& record, const CourseRemovalPlan& plan) override {
    if (record.phase != ContentRemovalPhase::Prepared || !select(record, plan, true) || !verifyBinding())
      return fail("plan binding/state");
    uint64_t length = 0;
    if (!guard() || metadata.stat(COURSE_REMOVAL_PROOF_STAGE, length) != FileStatus::Missing || !guard())
      return fail("unfinished proof publication");
    return (proofs.load(record.request.generation, proof) == CourseRemovalProofStorageResult::Missing && guard()) ||
           fail("existing removal proof");
  }
  bool publish(const ContentRemovalRecord& record, const CourseRemovalPlan& plan) override {
    return (record.phase == ContentRemovalPhase::Quarantined && select(record, plan) && verifyBinding() && guard() &&
            proofs.persist(record, journal) == CourseRemovalProofStorageResult::Ok && guard()) ||
           fail("proof publication");
  }
  bool verify(const ContentRemovalRecord& record, const CourseRemovalPlan& plan) override {
    return ((record.phase == ContentRemovalPhase::Quarantined || record.phase == ContentRemovalPhase::Committed) &&
            select(record, plan) && verifyBinding() && loadProof()) ||
           fail("published references");
  }
  bool retire(const ContentRemovalRecord& record, const CourseRemovalPlan& plan) override {
    return (record.phase == ContentRemovalPhase::Committed && verify(record, plan) && guard() &&
            baseline.retain(record, proof) && guard()) ||
           fail("baseline retention");
  }
  bool verifyRetired(const ContentRemovalRecord& record, const CourseRemovalPlan& plan) override {
    return (record.phase >= ContentRemovalPhase::Committed && select(record, plan) && verifyBinding() && loadProof() &&
            guard() && baseline.verify(record, proof) && guard()) ||
           fail("retained references");
  }
  bool closeReaders() {
    const bool stateClosed = isolation.closeReaders();
    const bool proofClosed = proofs.closeReaders();
    const bool baselineClosed = baseline.closeReaders();
    return stateClosed && proofClosed && baselineClosed;
  }

 private:
  ContentRemovalJournal& journal;
  TransferStorage& metadata;
  std::span<uint8_t> scratch;
  HalCourseStateIsolation& isolation;
  HalCourseRemovalProofStorage& proofs;
  HalCourseRemovalBaseline& baseline;
  Permission permitted;
  void* context;
  ContentRemovalRecord checkpoint, proof;
  ContentManifest binding;
  bool preflight = false;
  bool guard() const {
    return permitted && permitted(context) &&
           (preflight ? !journal.current() : journal.current() && *journal.current() == checkpoint);
  }
  bool select(const ContentRemovalRecord& record, const CourseRemovalPlan& plan, bool allowPreflight = false) {
    if (!validContentRemovalRecord(record) || !validCourseRemovalPlan(plan) || record.request != plan.request)
      return false;
    checkpoint = record;
    preflight = allowPreflight && !journal.current();
    return guard();
  }
  bool verifyBinding() {
    if (!guard()) return false;
    bool present = false;
    if (readCourseBinding(metadata, COURSE_BINDING_PATH, scratch, binding, present) != CourseBindingResult::Ok ||
        !guard() || !present || binding != checkpoint.request.manifest)
      return false;
    uint64_t length = 0;
    for (const auto* path : {COURSE_BINDING_STAGE, COURSE_BINDING_BACKUP})
      if (!guard() || metadata.stat(path, length) != FileStatus::Missing || !guard()) return false;
    return guard() && isolation.verify(binding.logicalIdentity) && guard();
  }
  bool loadProof() {
    return guard() && proofs.load(checkpoint.request.generation, proof) == CourseRemovalProofStorageResult::Ok &&
           guard() && matchesCourseRemovalProof(proof, checkpoint);
  }
  bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Course reference removal %s failed", operation);
    closeReaders();
    return false;
  }
};
}  // namespace companion

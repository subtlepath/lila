#pragma once

#include "CompanionContentRemoval.h"
#include "CompanionCourseRemovalPlan.h"

namespace companion {
class CourseRemovalStorage {
 public:
  virtual ~CourseRemovalStorage() = default;
  // Verify the sealed plan's SHA and its complete decoded value before binding.
  virtual bool verifyPlan(const CourseRemovalPlan&, const Digest&) = 0;
  // Member zero is the active course pack; other ordinals must be refused.
  virtual FileStatus stat(unsigned member, bool backup) = 0;
  virtual bool verify(unsigned member, bool backup, uint64_t length, const Digest& hash) = 0;
  virtual bool quarantine(unsigned member) = 0;
  virtual bool remove(unsigned member, bool backup) = 0;
};
class CourseRemovalReferences {
 public:
  virtual ~CourseRemovalReferences() = default;
  // Prove the bound course and durable isolation of its learner state.
  virtual bool verifyPlan(const ContentRemovalRecord&, const CourseRemovalPlan&) = 0;
  virtual bool publish(const ContentRemovalRecord&, const CourseRemovalPlan&) = 0;
  virtual bool verify(const ContentRemovalRecord&, const CourseRemovalPlan&) = 0;
  // Retain learner state, the verified pack baseline and bound removal proof.
  virtual bool retire(const ContentRemovalRecord&, const CourseRemovalPlan&) = 0;
  virtual bool verifyRetired(const ContentRemovalRecord&, const CourseRemovalPlan&) = 0;
};
// Retain outside the task stack. The sealed plan is immutable, disjoint from IO
// scratch and outlives this serialized participant; no allocations occur here.
class CourseRemovalParticipant final : public ContentRemovalParticipant {
 public:
  CourseRemovalParticipant(ContentRemovalJournal& journal, CourseRemovalStorage& storage,
                           CourseRemovalReferences& references)
      : journal(journal), storage(storage), references(references) {}
  bool bind(const CourseRemovalPlan& input, const Digest& hash) {
    if (plan || !validCourseRemovalPlan(input) ||
        !std::any_of(hash.begin(), hash.end(), [](uint8_t byte) { return byte != 0; }) ||
        (journal.current() && (journal.current()->request != input.request || journal.current()->planHash != hash)) ||
        !storage.verifyPlan(input, hash))
      return false;
    plan = &input;
    planHash = hash;
    return true;
  }
  // Release only borrowed context; the native storage owner closes its handle.
  void unbind() {
    plan = nullptr;
    planHash = {};
    preflight = false;
  }
  bool verifyPlan(const ContentRemovalRecord& r) override {
    if (r.phase != ContentRemovalPhase::Prepared || !select(r, true) || !references.verifyPlan(r, *plan) || !guard())
      return false;
    for (unsigned member = 0; member < count(); ++member) {
      if (!inspect(member)) return false;
      if (source == FileStatus::Present && backup == FileStatus::Missing) {
        if (!verify(member, false)) return false;
      } else if (preflight || backup != FileStatus::Present || !verify(member, true) ||
                 (source == FileStatus::Present && !verify(member, false)))
        return false;
    }
    return guard();
  }
  bool quarantine(const ContentRemovalRecord& r) override {
    if (r.phase != ContentRemovalPhase::Prepared || !select(r)) return false;
    for (unsigned member = 0; member < count(); ++member) {
      if (!inspect(member)) return false;
      if (backup == FileStatus::Present) {
        if (!verify(member, true)) return false;
        if (source == FileStatus::Present &&
            (!verify(member, false) || !guard() || !storage.remove(member, false) || !guard()))
          return false;
      } else if (source != FileStatus::Present || !verify(member, false) || !guard() || !storage.quarantine(member) ||
                 !guard())
        return false;
      if (!inspect(member) || source != FileStatus::Missing || backup != FileStatus::Present || !verify(member, true))
        return false;
    }
    return guard();
  }
  bool verifyQuarantined(const ContentRemovalRecord& r) override {
    if (r.phase > ContentRemovalPhase::Quarantined || !select(r)) return false;
    for (unsigned member = 0; member < count(); ++member)
      if (!inspect(member) || source != FileStatus::Missing || backup != FileStatus::Present || !verify(member, true))
        return false;
    return guard();
  }
  bool publishRemoval(const ContentRemovalRecord& r) override {
    return r.phase == ContentRemovalPhase::Quarantined && verifyQuarantined(r) && guard() &&
           references.publish(r, *plan) && guard();
  }
  bool verifyPublished(const ContentRemovalRecord& r) override {
    if ((r.phase != ContentRemovalPhase::Quarantined && r.phase != ContentRemovalPhase::Committed) || !select(r))
      return false;
    for (unsigned member = 0; member < count(); ++member)
      if (!inspect(member) || source != FileStatus::Missing ||
          (backup == FileStatus::Present && !verify(member, true)) ||
          (r.phase == ContentRemovalPhase::Quarantined && backup != FileStatus::Present))
        return false;
    return guard() && references.verify(r, *plan) && guard();
  }
  bool retireBackups(const ContentRemovalRecord& r) override { return retireReferences(r) && retireVerifiedBackups(r); }
  bool retireReferences(const ContentRemovalRecord& r) {
    return r.phase == ContentRemovalPhase::Committed && verifyPublished(r) && guard() && references.retire(r, *plan) &&
           guard() && references.verifyRetired(r, *plan) && guard();
  }
  bool retireVerifiedBackups(const ContentRemovalRecord& r) {
    if (r.phase != ContentRemovalPhase::Committed || !select(r) || !references.verifyRetired(r, *plan) || !guard() ||
        !verifyPublished(r))
      return false;
    for (unsigned member = 0; member < count(); ++member) {
      if (!inspect(member) || source != FileStatus::Missing) return false;
      if (backup == FileStatus::Present &&
          (!verify(member, true) || !guard() || !storage.remove(member, true) || !guard()))
        return false;
      if (!inspect(member) || backup != FileStatus::Missing) return false;
    }
    return guard();
  }
  bool verifyRetired(const ContentRemovalRecord& r) override {
    if (r.phase < ContentRemovalPhase::Committed || !select(r)) return false;
    for (unsigned member = 0; member < count(); ++member)
      if (!inspect(member) || source != FileStatus::Missing || backup != FileStatus::Missing) return false;
    return guard() && references.verifyRetired(r, *plan) && guard();
  }

 private:
  ContentRemovalJournal& journal;
  CourseRemovalStorage& storage;
  CourseRemovalReferences& references;
  const CourseRemovalPlan* plan = nullptr;
  Digest planHash{};
  ContentRemovalRecord checkpoint;
  FileStatus source = FileStatus::Error, backup = FileStatus::Error;
  bool preflight = false;
  static constexpr unsigned count() { return 1; }
  bool guard() const { return preflight ? !journal.current() : journal.current() && *journal.current() == checkpoint; }
  bool select(const ContentRemovalRecord& r, bool allowPreflight = false) {
    if (!plan || !validContentRemovalRecord(r) || r.request != plan->request || r.planHash != planHash) return false;
    checkpoint = r;
    preflight = allowPreflight && !journal.current();
    return guard();
  }
  bool inspect(unsigned member) {
    if (!guard()) return false;
    source = storage.stat(member, false);
    if (!guard() || source == FileStatus::Error) return false;
    backup = storage.stat(member, true);
    return guard() && backup != FileStatus::Error;
  }
  bool verify(unsigned member, bool saved) {
    return guard() &&
           storage.verify(member, saved, plan->request.manifest.length, plan->request.manifest.contentHash) && guard();
  }
};
}  // namespace companion

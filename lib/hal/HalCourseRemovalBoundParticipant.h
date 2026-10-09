#pragma once

#include "HalCourseRemovalPlanStorage.h"
#include "HalCourseRemovalStorage.h"

namespace companion {
// Fixed sealed bytes and decoded plan remain off stack. Every checkpoint reloads
// the SHA-addressed plan before lending it to the payload participant.
class HalCourseRemovalBoundParticipant final : public ContentRemovalParticipant {
 public:
  HalCourseRemovalBoundParticipant(ContentRemovalJournal& journal, HalCourseRemovalPlanStorage& plans,
                                   CourseRemovalReferences& references, std::span<uint8_t> io,
                                   InventoryHashProgress permitted, void* context)
      : journal(journal),
        plans(plans),
        members(journal, bytes, io),
        worker(journal, members, references),
        permitted(permitted),
        context(context) {}
  bool closeReaders() {
    worker.unbind();
    return members.unbind();
  }
  bool verifyPlan(const ContentRemovalRecord& r) override { return run(r, &CourseRemovalParticipant::verifyPlan); }
  bool quarantine(const ContentRemovalRecord& r) override { return run(r, &CourseRemovalParticipant::quarantine); }
  bool verifyQuarantined(const ContentRemovalRecord& r) override {
    return run(r, &CourseRemovalParticipant::verifyQuarantined);
  }
  bool publishRemoval(const ContentRemovalRecord& r) override {
    return run(r, &CourseRemovalParticipant::publishRemoval);
  }
  bool verifyPublished(const ContentRemovalRecord& r) override {
    return run(r, &CourseRemovalParticipant::verifyPublished);
  }
  bool retireBackups(const ContentRemovalRecord& r) override {
    return run(r, &CourseRemovalParticipant::retireBackups);
  }
  bool verifyRetired(const ContentRemovalRecord& r) override {
    return run(r, &CourseRemovalParticipant::verifyRetired);
  }

 private:
  ContentRemovalJournal& journal;
  HalCourseRemovalPlanStorage& plans;
  std::array<uint8_t, COURSE_REMOVAL_PLAN_SIZE> bytes{};
  CourseRemovalPlan plan;
  HalCourseRemovalStorage members;
  CourseRemovalParticipant worker;
  InventoryHashProgress permitted;
  void* context;
  ContentRemovalRecord checkpoint;
  bool preflight = false;
  bool guard() const {
    return permitted && permitted(context) &&
           (preflight ? !journal.current() : journal.current() && *journal.current() == checkpoint);
  }
  bool run(const ContentRemovalRecord& record,
           bool (CourseRemovalParticipant::*operation)(const ContentRemovalRecord&)) {
    checkpoint = record;
    preflight = !journal.current();
    if (!validContentRemovalRecord(record) ||
        (preflight &&
         (record.phase != ContentRemovalPhase::Prepared || operation != &CourseRemovalParticipant::verifyPlan)) ||
        !guard() || !closeReaders() || !guard() ||
        plans.load(record.planHash, bytes, plan) != CourseRemovalPlanStorageResult::Ok || !guard() ||
        plan.request != record.request || !worker.bind(plan, record.planHash) || !guard()) {
      closeReaders();
      return fail();
    }
    const bool result = (worker.*operation)(record);
    const bool stillOwned = guard();
    const bool closed = closeReaders();
    return (result && stillOwned && closed && guard()) || fail();
  }
  static bool fail() {
    LOG_ERR("COMPANION", "Course removal plan binding failed");
    return false;
  }
};
}  // namespace companion

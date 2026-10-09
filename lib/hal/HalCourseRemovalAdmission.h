#pragma once

#include "CompanionContentRemovalAdmission.h"
#include "HalContentRemovalJournalStorage.h"
#include "HalCourseRemovalPlanStorage.h"

namespace companion {
// The caller authenticates the peer, validates the complete inventory pair and
// excludes all content/state writers. Retain this fixed owner off stack.
class HalCourseRemovalAdmission final : public ContentRemovalAdmission {
 public:
  using InventoryReady = bool (*)(void*, uint64_t&);
  using Permission = bool (*)(void*);
  using PrepareState = bool (*)(void*, const ContentManifest&);
  HalCourseRemovalAdmission(const Identity& generation, ContentRemovalJournal& journal, InventoryPaths& paths,
                            HalCourseRemovalPlanStorage& plans, InventoryReady inventoryReady,
                            PrepareState prepareState, Permission permitted, void* context)
      : generation(generation),
        journal(journal),
        paths(paths),
        plans(plans),
        inventoryReady(inventoryReady),
        prepareState(prepareState),
        permitted(permitted),
        context(context) {}
  bool supports(ContentKind kind) const override { return kind == ContentKind::Course; }
  EpubRemovalAdmissionResult admit(const ContentRemovalRequest& request, uint64_t revision,
                                   ContentRemovalRecord& output) override {
    plan.request = request;
    if (!validCourseRemovalPlan(plan)) return EpubRemovalAdmissionResult::Invalid;
    if (request.generation != generation) return EpubRemovalAdmissionResult::WrongStorage;
    if (!guard()) return EpubRemovalAdmissionResult::Busy;
    const auto recovered = journal.recover(generation);
    if (!guard()) return EpubRemovalAdmissionResult::Busy;
    if (recovered == ContentRemovalJournalResult::Ok) {
      candidate = *journal.current();
      if (candidate.request != request) return EpubRemovalAdmissionResult::Busy;
      if (candidate.phase == ContentRemovalPhase::Retired) {
        candidate.phase = ContentRemovalPhase::Prepared;
        candidate.revision = 1;
        output = candidate;
        return EpubRemovalAdmissionResult::Retired;
      }
    } else if (recovered == ContentRemovalJournalResult::Missing) {
      if (!inventoryReady || !inventoryReady(context, revision) || !guard() || !revision ||
          !paths.open(generation, revision))
        return io("inventory preparation");
      bool found = false;
      for (;;) {
        if (!guard()) return EpubRemovalAdmissionResult::Busy;
        const auto result = paths.nextPath(manifest, path);
        if (!guard()) return EpubRemovalAdmissionResult::Busy;
        if (result == InventoryPathRecordResult::Error) return io("inventory iteration");
        if (result == InventoryPathRecordResult::End) break;
        if (manifest.contentHash != request.manifest.contentHash) continue;
        if (found || manifest != request.manifest || std::strcmp(path.data(), ACTIVE_COURSE_PATH) != 0)
          return EpubRemovalAdmissionResult::Conflict;
        found = true;
      }
      if (!found) return EpubRemovalAdmissionResult::NotFound;
      if (!prepareState || !prepareState(context, request.manifest) || !guard() || journal.current())
        return io("learner state preparation");
      candidate = {};
      candidate.request = request;
      if (CourseRemovalPlanCodec::encode(plan, bytes) != bytes.size()) return EpubRemovalAdmissionResult::Invalid;
      const auto published = plans.publish(bytes, revision, candidate.planHash);
      if (!guard()) return EpubRemovalAdmissionResult::Busy;
      if (published != CourseRemovalPlanStorageResult::Ok) return map(published);
    } else {
      if (recovered == ContentRemovalJournalResult::Conflict) return EpubRemovalAdmissionResult::Conflict;
      return recovered == ContentRemovalJournalResult::Corrupt ? EpubRemovalAdmissionResult::Corrupt : io("journal");
    }
    const auto loaded = plans.load(candidate.planHash, bytes, plan);
    if (!guard()) return EpubRemovalAdmissionResult::Busy;
    if (loaded != CourseRemovalPlanStorageResult::Ok) return map(loaded);
    if (plan.request != request) return EpubRemovalAdmissionResult::Conflict;
    candidate.phase = ContentRemovalPhase::Prepared;
    candidate.revision = 1;
    output = candidate;
    return EpubRemovalAdmissionResult::Ready;
  }

 private:
  Identity generation;
  ContentRemovalJournal& journal;
  InventoryPaths& paths;
  HalCourseRemovalPlanStorage& plans;
  InventoryReady inventoryReady;
  PrepareState prepareState;
  Permission permitted;
  void* context;
  CourseRemovalPlan plan;
  std::array<uint8_t, COURSE_REMOVAL_PLAN_SIZE> bytes{};
  ContentRemovalRecord candidate;
  ContentManifest manifest;
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  bool guard() const { return permitted && permitted(context); }
  static EpubRemovalAdmissionResult map(CourseRemovalPlanStorageResult result) {
    if (result == CourseRemovalPlanStorageResult::Conflict) return EpubRemovalAdmissionResult::Conflict;
    if (result == CourseRemovalPlanStorageResult::Corrupt) return EpubRemovalAdmissionResult::Corrupt;
    if (result == CourseRemovalPlanStorageResult::Invalid) return EpubRemovalAdmissionResult::Invalid;
    return io("sealed plan");
  }
  static EpubRemovalAdmissionResult io(const char* operation) {
    LOG_ERR("COMPANION", "Course removal admission %s failed", operation);
    return EpubRemovalAdmissionResult::IoError;
  }
};
}  // namespace companion

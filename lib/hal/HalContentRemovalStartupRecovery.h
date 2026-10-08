#pragma once

#include <Logging.h>
#include <Memory.h>

#include "HalContentRemovalTransactions.h"
#include "HalEpubRemovalCohortParticipant.h"
#include "HalEpubRemovalReferences.h"
#include "HalSingleFileRemovalPlanStorage.h"

namespace companion {
// Boot-only owner: no reader/store writers may run until recovery completes.
class HalContentRemovalStartupRecovery final {
 public:
  HalContentRemovalStartupRecovery()
      : journal(storage, journalBytes),
        completions(completionBytes),
        release(journal, storage, completions, releaseBytes),
        plans(io),
        references(journal, declarationBytes, io),
        participant(journal, references, io),
        removal(journal, participant) {}
  bool pending(bool& output) {
    output = false;
    if (!storage.prepare()) return failure("journal directory");
    for (const char* path : CONTENT_REMOVAL_JOURNALS) {
      uint64_t size = 0;
      const auto status = storage.stat(path, size);
      if (status == FileStatus::Error) return failure("journal lookup");
      output |= status == FileStatus::Present;
    }
    return true;
  }
  bool run(const Identity& generation) {
    const auto recovered = journal.recover(generation);
    if (recovered == ContentRemovalJournalResult::Missing) return true;
    if (recovered != ContentRemovalJournalResult::Ok) return failure("journal");
    checkpoint = *journal.current();
    auto completed = completions.load(checkpoint.request, receipt);
    if (!unchanged()) return failure("ownership");
    if (completed == CompletedRemovalResult::Missing && checkpoint.phase == ContentRemovalPhase::Retired)
      completed = completions.persist(checkpoint, journal);
    if (completed == CompletedRemovalResult::Ok)
      return release.release() == CompletedRemovalResult::Ok || failure("release");
    if (completed != CompletedRemovalResult::Missing) return failure("completion receipt");
    if (checkpoint.request.manifest.kind != ContentKind::Epub) return failure("unsupported participant");
    const auto loaded = plans.load(checkpoint.planHash, planBytes, plan);
    if (!unchanged()) return failure("persisted plan ownership");
    initial = checkpoint;
    initial.phase = ContentRemovalPhase::Prepared;
    initial.revision = 1;
    if (loaded == RemovalPlanStorageResult::Ok) {
      if (plan.request != checkpoint.request) return failure("persisted request");
      const auto length = SINGLE_FILE_REMOVAL_PLAN_PREFIX + plan.path.size() + 4;
      if (!participant.bind(std::span(planBytes).first(length), checkpoint.planHash) ||
          removal.remove(initial) != ContentRemovalJournalResult::Ok)
        return failure("participant recovery");
    } else if (loaded == RemovalPlanStorageResult::Corrupt) {
      // The LRMP reader independently verifies format, full request and SHA.
      // Allocate its reusable worker/buffers off stack only for cohort recovery.
      auto cohort = makeUniqueNoThrow<CohortRecovery>(journal, references, io);
      if (!cohort) return failure("OOM: cohort recovery");
      if (cohort->plans.open(checkpoint.planHash, checkpoint.request) != MultiPathRemovalStorageResult::Ok ||
          !unchanged() || cohort->removal.remove(initial) != ContentRemovalJournalResult::Ok)
        return failure("cohort recovery");
    } else {
      return failure("persisted plan");
    }
    checkpoint = *journal.current();
    if (completions.persist(checkpoint, journal) != CompletedRemovalResult::Ok)
      return failure("completion publication");
    return release.release() == CompletedRemovalResult::Ok || failure("release");
  }

 private:
  struct CohortRecovery {
    HalMultiPathRemovalPlanStorage plans;
    HalEpubRemovalCohortParticipant participant;
    ContentRemoval removal;
    CohortRecovery(ContentRemovalJournal& journal, HalEpubRemovalReferences& references, std::span<uint8_t> io)
        : participant(journal, plans, references, io), removal(journal, participant) {}
  };
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{}, completionBytes{}, releaseBytes{};
  std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> declarationBytes{};
  std::array<uint8_t, SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE> planBytes{};
  std::array<uint8_t, 128> io{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal;
  HalCompletedContentRemovals completions;
  HalCompletedRemovalJournalRelease release;
  HalSingleFileRemovalPlanStorage plans;
  HalEpubRemovalReferences references;
  HalEpubRemovalParticipant participant;
  ContentRemoval removal;
  ContentRemovalRecord checkpoint, receipt, initial;
  SingleFileRemovalPlan plan;
  bool unchanged() const { return journal.current() && *journal.current() == checkpoint; }
  static bool failure(const char* stage) {
    LOG_ERR("COMPANION", "Removal startup recovery failed: %s", stage);
    return false;
  }
};
}  // namespace companion

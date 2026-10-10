#pragma once

#include "HalCourseRemovalAdmission.h"
#include "HalCourseRemovalBoundParticipant.h"
#include "HalCourseRemovalReferences.h"
#include "HalEpubRemovalBackend.h"

namespace companion {
// Allocate once with makeUniqueNoThrow after heap admission. Fixed metadata and
// handle owners exceed task-stack limits; hashing/migration borrow session IO.
class HalCourseRemovalSession final {
 public:
  HalCourseRemovalSession(const Identity& generation, InventoryPaths& paths, const uint64_t& revision,
                          TransferStorage& metadata, std::span<uint8_t> io, HalEpubRemovalBackend::Callback permitted,
                          HalEpubRemovalBackend::Callback refresh, void* context,
                          HalEpubRemovalAdmission::InventoryReady inventoryReady,
                          HalCourseRemovalAdmission::PrepareState prepareState)
      : generation(generation),
        io(io),
        permitted(permitted),
        context(context),
        journal(storage, journalBytes),
        completions(completionBytes),
        plans(comparisonBytes),
        isolation(metadata, io, permitted, context),
        proofs(proofBytes, permitted, context),
        baseline(journal, io, permitted, context),
        references(journal, metadata, io, isolation, proofs, baseline, permitted, context),
        participant(journal, plans, references, io, permitted, context),
        transactions(generation, journal, storage, completions, participant, releaseBytes),
        admission(generation, journal, paths, plans, inventoryReady, prepareState, permitted, context),
        backend(admission, transactions, revision, permitted, refresh, context),
        handler(backend) {}
  bool prepare() {
    prepared = false;
    if (io.size() < 512 || !permitted || !permitted(context) || !storage.prepare() || !permitted(context)) {
      LOG_ERR("COMPANION", "Course removal session preparation failed");
      return false;
    }
    prepared = true;
    return true;
  }
  bool closeReaders() {
    const bool participantClosed = participant.closeReaders();
    const bool referenceClosed = references.closeReaders();
    const bool planClosed = plans.closeReaders();
    return participantClosed && referenceClosed && planClosed;
  }
  bool finishCompleted() {
    if (!prepared || !permitted || !permitted(context) || !closeReaders() ||
        transactions.finishCompleted() != ContentRemovalJournalResult::Ok || !permitted(context)) {
      LOG_ERR("COMPANION", "Course removal completion release failed");
      return false;
    }
    return true;
  }
  size_t handle(bool authorized, const Identity& owner, std::span<const uint8_t> request, std::span<uint8_t> reply) {
    return prepared ? handler.handle(authorized, owner, generation, request, reply) : 0;
  }

 private:
  Identity generation;
  std::span<uint8_t> io;
  HalEpubRemovalBackend::Callback permitted;
  void* context;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{}, completionBytes{}, releaseBytes{}, proofBytes{};
  std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> comparisonBytes{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal;
  HalCompletedContentRemovals completions;
  HalCourseRemovalPlanStorage plans;
  HalCourseStateIsolation isolation;
  HalCourseRemovalProofStorage proofs;
  HalCourseRemovalBaseline baseline;
  HalCourseRemovalReferences references;
  HalCourseRemovalBoundParticipant participant;
  HalContentRemovalTransactions transactions;
  HalCourseRemovalAdmission admission;
  HalEpubRemovalBackend backend;
  ContentRemovalHandler handler;
  bool prepared = false;
};
}  // namespace companion

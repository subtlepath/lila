#pragma once

#include "HalCourseRemovalBoundParticipant.h"
#include "HalCourseRemovalReferences.h"

namespace companion {
// Boot-only retained owner. Borrow metadata and IO while all reader/state writers
// remain stopped; parent recovery publishes the receipt and releases the journal.
class HalCourseRemovalRecovery final {
 public:
  using Permission = bool (*)(void*);
  HalCourseRemovalRecovery(ContentRemovalJournal& journal, TransferStorage& metadata, std::span<uint8_t> io,
                           Permission permitted, void* context)
      : journal(journal),
        io(io),
        permitted(permitted),
        context(context),
        plans(comparisonBytes),
        isolation(metadata, io, permitted, context),
        proofs(proofBytes, permitted, context),
        baseline(journal, io, permitted, context),
        references(journal, metadata, io, isolation, proofs, baseline, permitted, context),
        participant(journal, plans, references, io, permitted, context),
        removal(journal, participant) {}
  bool run(const ContentRemovalRecord& checkpoint) {
    if (io.size() < COURSE_BINDING_SIZE || !permitted || !permitted(context) ||
        !validContentRemovalRecord(checkpoint) || checkpoint.request.manifest.kind != ContentKind::Course ||
        !journal.current() || *journal.current() != checkpoint)
      return fail("admission");
    initial = checkpoint;
    initial.phase = ContentRemovalPhase::Prepared;
    initial.revision = 1;
    const auto result = removal.remove(initial);
    const bool closed = closeReaders();
    return (result == ContentRemovalJournalResult::Ok && closed && permitted(context) && journal.current() &&
            journal.current()->request == initial.request && journal.current()->planHash == initial.planHash &&
            journal.current()->phase == ContentRemovalPhase::Retired) ||
           fail("checkpoint completion");
  }
  bool closeReaders() {
    const bool participantClosed = participant.closeReaders();
    const bool referenceClosed = references.closeReaders();
    const bool planClosed = plans.closeReaders();
    return participantClosed && referenceClosed && planClosed;
  }

 private:
  ContentRemovalJournal& journal;
  std::span<uint8_t> io;
  Permission permitted;
  void* context;
  std::array<uint8_t, REMOVAL_STAGE_CLAIM_SIZE> comparisonBytes{};
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> proofBytes{};
  HalCourseRemovalPlanStorage plans;
  HalCourseStateIsolation isolation;
  HalCourseRemovalProofStorage proofs;
  HalCourseRemovalBaseline baseline;
  HalCourseRemovalReferences references;
  HalCourseRemovalBoundParticipant participant;
  ContentRemoval removal;
  ContentRemovalRecord initial;
  bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Course removal boot recovery %s failed", operation);
    closeReaders();
    return false;
  }
};
}  // namespace companion

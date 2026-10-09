#pragma once

#include "HalEpubRemovalBackend.h"
#include "HalEpubRemovalCohortParticipant.h"
#include "HalRemovalParticipantRouter.h"

namespace companion {
// Allocate this retained session with makeUniqueNoThrow: its fixed buffers and
// handles exceed the task stack. Borrowed paths/revision/callbacks outlive it.
class HalEpubRemovalSession final {
 public:
  HalEpubRemovalSession(const Identity& generation, InventoryPaths& paths, const uint64_t& revision,
                        uint64_t maximumPlanBytes, HalEpubRemovalBackend::Callback permitted,
                        HalEpubRemovalBackend::Callback refresh, void* context,
                        HalEpubRemovalAdmission::InventoryReady inventoryReady = nullptr,
                        FontRemovalSettings* fontSettings = nullptr)
      : generation(generation),
        journal(storage, journalBytes),
        completions(completionBytes),
        writer(maximumPlanBytes),
        collection(paths, writer),
        references(journal, declarationBytes, io),
        cohort(journal, plans, references, io),
        participants(cohort, journal, plans, io, fontSettings),
        transactions(generation, journal, storage, completions, participants, releaseBytes),
        admission(generation, journal, collection, writer, plans, inventoryReady, context),
        backend(admission, transactions, revision, permitted, refresh, context),
        handler(backend) {}
  // Call only after authenticated session admission and exclusion of writers.
  bool prepare() { return storage.prepare(); }
  bool closeReaders() { return plans.close(); }
  bool setPlanQuota(uint64_t bytes) { return writer.setMaximumBytes(bytes); }
  size_t handle(bool authorized, const Identity& owner, std::span<const uint8_t> request, std::span<uint8_t> reply) {
    return handler.handle(authorized, owner, generation, request, reply);
  }

 private:
  Identity generation;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{}, completionBytes{}, releaseBytes{};
  std::array<uint8_t, REMOVAL_METADATA_SNAPSHOT_SIZE> declarationBytes{};
  std::array<uint8_t, 128> io{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal;
  HalCompletedContentRemovals completions;
  HalMultiPathRemovalPlanWriter writer;
  RemovalPathCollection collection;
  HalMultiPathRemovalPlanStorage plans;
  HalEpubRemovalReferences references;
  HalEpubRemovalCohortParticipant cohort;
  HalRemovalParticipantRouter participants;
  HalContentRemovalTransactions transactions;
  HalEpubRemovalAdmission admission;
  HalEpubRemovalBackend backend;
  ContentRemovalHandler handler;
};
}  // namespace companion

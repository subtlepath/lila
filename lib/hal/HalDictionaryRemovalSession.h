#pragma once

#include "HalDictionaryCacheStorage.h"
#include "HalDictionaryRemovalAdmission.h"
#include "HalDictionaryRemovalCohortParticipant.h"
#include "HalDictionaryRemovalReferences.h"
#include "HalEpubRemovalBackend.h"

namespace companion {
// Allocate once with makeUniqueNoThrow after internal-heap admission. Large fixed
// plan/decoder state exceeds task-stack limits; IO borrows the session workspace.
class HalDictionaryRemovalSession final {
 public:
  HalDictionaryRemovalSession(const Identity& generation, InventoryPaths& paths, const uint64_t& revision,
                              uint64_t maximumPlanBytes, std::span<uint8_t> io, DictionaryRemovalSettings& settings,
                              HalEpubRemovalBackend::Callback permitted, HalEpubRemovalBackend::Callback refresh,
                              void* context, HalEpubRemovalAdmission::InventoryReady inventoryReady = nullptr)
      : generation(generation),
        io(io),
        journal(storage, journalBytes),
        completions(completionBytes),
        cache(permitted, context),
        assembly(cache, io, permitted, context),
        writer(maximumPlanBytes),
        collection(assembly, writer, permitted, context),
        bindings(cache, io, permitted, context),
        references(journal, settings, bindings),
        cohort(journal, plans, references, io),
        transactions(generation, journal, storage, completions, cohort, releaseBytes),
        admission(generation, journal, paths, collection, writer, plans, inventoryReady, permitted, context),
        backend(admission, transactions, revision, permitted, refresh, context),
        handler(backend) {}
  bool prepare() {
    prepared = false;
    if (io.size() < DICTIONARY_BINDING_SIZE) {
      LOG_ERR("COMPANION", "Dictionary removal IO workspace too short");
      return false;
    }
    prepared = storage.prepare();
    return prepared;
  }
  bool setPlanQuota(uint64_t bytes) { return writer.setMaximumBytes(bytes); }
  bool closeReaders() {
    const bool cohortClosed = cohort.closeReaders();
    const bool planClosed = plans.close();
    const bool writerClosed = writer.releaseHandles();
    const bool collectionClosed = collection.closeReaders();
    const bool assemblyClosed = assembly.closeReaders();
    const bool bindingClosed = bindings.closeReaders();
    const bool cacheClosed = cache.closeReaders();
    return cohortClosed && planClosed && writerClosed && collectionClosed && assemblyClosed && bindingClosed &&
           cacheClosed;
  }
  size_t handle(bool authorized, const Identity& owner, std::span<const uint8_t> request, std::span<uint8_t> reply) {
    return prepared ? handler.handle(authorized, owner, generation, request, reply) : 0;
  }

 private:
  Identity generation;
  std::span<uint8_t> io;
  std::array<uint8_t, CONTENT_REMOVAL_RECORD_SIZE> journalBytes{}, completionBytes{}, releaseBytes{};
  HalContentRemovalJournalStorage storage;
  ContentRemovalJournal journal;
  HalCompletedContentRemovals completions;
  HalDictionaryCacheStorage cache;
  HalDictionaryRemovalPlanAssembly assembly;
  HalDictionaryRemovalCohortPlanWriter writer;
  HalDictionaryRemovalPlanCollection collection;
  HalDictionaryRemovalCohortPlanStorage plans;
  HalDictionaryBindings bindings;
  HalDictionaryRemovalReferences references;
  HalDictionaryRemovalCohortParticipant cohort;
  HalContentRemovalTransactions transactions;
  HalDictionaryRemovalAdmission admission;
  HalEpubRemovalBackend backend;
  ContentRemovalHandler handler;
  bool prepared = false;
};
}  // namespace companion

#pragma once

#include "HalContentRemovalJournalStorage.h"
#include "HalMultiPathRemovalPlanStorage.h"
#include "HalMultiPathRemovalPlanWriter.h"

namespace companion {
enum class EpubRemovalAdmissionResult {
  Ready,
  Retired,
  Invalid,
  WrongStorage,
  Busy,
  NotFound,
  Conflict,
  Corrupt,
  IoError
};
// Off-stack owner. Authentication and completed-receipt lookup precede admission.
// The caller validates the inventory pair and excludes all logical writers.
class HalEpubRemovalAdmission final {
 public:
  using InventoryReady = bool (*)(void*, uint64_t& revision);
  HalEpubRemovalAdmission(const Identity& generation, ContentRemovalJournal& journal, RemovalPathCollection& collection,
                          HalMultiPathRemovalPlanWriter& writer, HalMultiPathRemovalPlanStorage& plans,
                          InventoryReady inventoryReady = nullptr, void* context = nullptr)
      : generation(generation),
        journal(journal),
        collection(collection),
        writer(writer),
        plans(plans),
        inventoryReady(inventoryReady),
        context(context) {}
  EpubRemovalAdmissionResult admit(const ContentRemovalRequest& request, uint64_t inventoryRevision,
                                   ContentRemovalRecord& output) {
    if (!validContentRemovalRequest(request) || request.manifest.kind != ContentKind::Epub)
      return EpubRemovalAdmissionResult::Invalid;
    expected = request;
    if (expected.generation != generation) return EpubRemovalAdmissionResult::WrongStorage;
    const auto recovered = journal.recover(generation);
    if (recovered == ContentRemovalJournalResult::Ok) {
      candidate = *journal.current();
      if (candidate.request != expected) return EpubRemovalAdmissionResult::Busy;
      if (candidate.phase == ContentRemovalPhase::Retired) {
        candidate.phase = ContentRemovalPhase::Prepared;
        candidate.revision = 1;
        output = candidate;
        return EpubRemovalAdmissionResult::Retired;
      }
    } else if (recovered == ContentRemovalJournalResult::Missing) {
      if (inventoryReady && !inventoryReady(context, inventoryRevision)) return io("inventory preparation");
      const auto collected = collection.collect(expected, inventoryRevision);
      if (collected != RemovalPathCollectionResult::Ok) {
        switch (collected) {
          case RemovalPathCollectionResult::Missing:
            return EpubRemovalAdmissionResult::NotFound;
          case RemovalPathCollectionResult::Invalid:
            return EpubRemovalAdmissionResult::Invalid;
          case RemovalPathCollectionResult::Conflict:
            return EpubRemovalAdmissionResult::Conflict;
          default:
            return io("collection");
        }
      }
      if (!writer.publishedDigest()) return io("publication");
      candidate = {};
      candidate.request = expected;
      candidate.planHash = *writer.publishedDigest();
    } else {
      if (recovered == ContentRemovalJournalResult::Conflict) return EpubRemovalAdmissionResult::Conflict;
      return recovered == ContentRemovalJournalResult::Corrupt ? EpubRemovalAdmissionResult::Corrupt : io("journal");
    }
    const auto loaded = plans.open(candidate.planHash, expected);
    if (loaded != MultiPathRemovalStorageResult::Ok)
      return loaded == MultiPathRemovalStorageResult::Corrupt ? EpubRemovalAdmissionResult::Corrupt : io("plan");
    candidate.phase = ContentRemovalPhase::Prepared;
    candidate.revision = 1;
    output = candidate;
    return EpubRemovalAdmissionResult::Ready;
  }

 private:
  Identity generation;
  ContentRemovalJournal& journal;
  RemovalPathCollection& collection;
  HalMultiPathRemovalPlanWriter& writer;
  HalMultiPathRemovalPlanStorage& plans;
  InventoryReady inventoryReady;
  void* context;
  ContentRemovalRequest expected;
  ContentRemovalRecord candidate;
  static EpubRemovalAdmissionResult io(const char* operation) {
    LOG_ERR("COMPANION", "EPUB removal admission %s failed", operation);
    return EpubRemovalAdmissionResult::IoError;
  }
};
}  // namespace companion

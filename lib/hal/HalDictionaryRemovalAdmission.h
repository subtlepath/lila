#pragma once

#include "HalDictionaryRemovalCohortPlanStorage.h"
#include "HalDictionaryRemovalPlanCollection.h"
#include "HalEpubRemovalAdmission.h"

namespace companion {
// Authentication, complete inventory-pair validation and writer exclusion precede
// admission. Retain outside the stack; all referenced owners outlive this one.
class HalDictionaryRemovalAdmission final : public ContentRemovalAdmission {
 public:
  HalDictionaryRemovalAdmission(const Identity& generation, ContentRemovalJournal& journal, InventoryPaths& paths,
                                HalDictionaryRemovalPlanCollection& collection,
                                HalDictionaryRemovalCohortPlanWriter& writer,
                                HalDictionaryRemovalCohortPlanStorage& plans,
                                HalEpubRemovalAdmission::InventoryReady inventoryReady, InventoryHashProgress progress,
                                void* context)
      : generation(generation),
        journal(journal),
        paths(paths),
        collection(collection),
        writer(writer),
        plans(plans),
        inventoryReady(inventoryReady),
        progress(progress),
        context(context) {}
  bool supports(ContentKind kind) const override { return kind == ContentKind::Dictionary; }
  EpubRemovalAdmissionResult admit(const ContentRemovalRequest& request, uint64_t revision,
                                   ContentRemovalRecord& output) override {
    if (!validContentRemovalRequest(request) || !supports(request.manifest.kind))
      return EpubRemovalAdmissionResult::Invalid;
    if (request.generation != generation) return EpubRemovalAdmissionResult::WrongStorage;
    if (!guard()) return EpubRemovalAdmissionResult::Busy;
    expected = request;
    const auto recovered = journal.recover(generation);
    if (!guard()) return EpubRemovalAdmissionResult::Busy;
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
      if (inventoryReady && !inventoryReady(context, revision)) return io("inventory preparation");
      if (!guard() || !revision || !paths.open(generation, revision)) return io("inventory paths");
      bool found = false;
      for (;;) {
        if (!guard()) return EpubRemovalAdmissionResult::Busy;
        const auto result = paths.nextPath(manifest, path);
        if (!guard()) return EpubRemovalAdmissionResult::Busy;
        if (result == InventoryPathRecordResult::Error) return io("inventory iteration");
        if (result == InventoryPathRecordResult::End) break;
        if (manifest.contentHash != expected.manifest.contentHash) continue;
        if (manifest != expected.manifest) return EpubRemovalAdmissionResult::Conflict;
        found = true;
      }
      if (!found) return EpubRemovalAdmissionResult::NotFound;
      if (!collection.collect(expected, revision) || !writer.publishedDigest() || !guard())
        return io("collection/publication");
      candidate = {};
      candidate.request = expected;
      candidate.planHash = *writer.publishedDigest();
    } else {
      if (recovered == ContentRemovalJournalResult::Conflict) return EpubRemovalAdmissionResult::Conflict;
      return recovered == ContentRemovalJournalResult::Corrupt ? EpubRemovalAdmissionResult::Corrupt : io("journal");
    }
    const auto loaded = plans.open(candidate.planHash, expected);
    if (!guard()) return EpubRemovalAdmissionResult::Busy;
    if (loaded != DictionaryRemovalCohortStorageResult::Ok)
      return loaded == DictionaryRemovalCohortStorageResult::Corrupt ? EpubRemovalAdmissionResult::Corrupt : io("plan");
    candidate.phase = ContentRemovalPhase::Prepared;
    candidate.revision = 1;
    output = candidate;
    return EpubRemovalAdmissionResult::Ready;
  }

 private:
  Identity generation;
  ContentRemovalJournal& journal;
  InventoryPaths& paths;
  HalDictionaryRemovalPlanCollection& collection;
  HalDictionaryRemovalCohortPlanWriter& writer;
  HalDictionaryRemovalCohortPlanStorage& plans;
  HalEpubRemovalAdmission::InventoryReady inventoryReady;
  InventoryHashProgress progress;
  void* context;
  ContentRemovalRequest expected;
  ContentRemovalRecord candidate;
  ContentManifest manifest;
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  bool guard() const { return progress && progress(context); }
  static EpubRemovalAdmissionResult io(const char* reason) {
    LOG_ERR("COMPANION", "Dictionary removal admission %s failed", reason);
    return EpubRemovalAdmissionResult::IoError;
  }
};
}  // namespace companion

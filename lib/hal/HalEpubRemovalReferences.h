#pragma once

#include <Memory.h>

#include <optional>

#include "HalEpubReferenceSnapshot.h"
#include "HalEpubRemovalParticipant.h"
#include "HalRemovalMetadataAuthorization.h"
#include "HalRemovalMetadataPublisher.h"

namespace companion {
// One owner per removal transaction. UI/store writers remain suspended until the
// operation finishes. IO and declaration scratch remain separate and exclusive.
class HalEpubRemovalReferences final : public EpubRemovalReferences {
 public:
  HalEpubRemovalReferences(ContentRemovalJournal& journal, std::span<uint8_t> declarationScratch,
                           std::span<uint8_t> ioScratch)
      : journal(journal),
        declarationScratch(declarationScratch),
        ioScratch(ioScratch),
        declarations(declarationScratch),
        authorization(journal, declarations) {
    for (auto& publisher : publications)
      publisher.emplace(ioScratch, HalRemovalMetadataAuthorization::check, &authorization);
  }
  bool publish(const ContentRemovalRecord& record, const char* path) override {
    if (!select(record, path)) return failure("publish context");
    return publishMatching(
        record,
        [](void* opaque, std::string_view candidate, bool& matched) {
          matched = removalReferencePathEqual(nullptr, candidate, static_cast<const char*>(opaque));
          return true;
        },
        const_cast<char*>(path));
  }
  bool publishMatching(const ContentRemovalRecord& record, EpubReferenceJson::PathMatch match, void* context) {
    if (!match || !selectOwner(record) || record.phase != ContentRemovalPhase::Quarantined)
      return failure("publish context");
    bool missing = false;
    for (size_t i = 0; i < 2; ++i) {
      const auto result = declarations.load(record.planHash, kind(i), snapshots[i]);
      pending[i] = result == RemovalMetadataStorageResult::Missing;
      if (!guard() || (result != RemovalMetadataStorageResult::Ok && !pending[i]) || (!pending[i] && !owned(i)))
        return failure("declaration ownership");
      missing |= pending[i];
    }
    if (missing) {
      // The JSON arena/preparer exceed the task stack. Allocate once for both
      // snapshots and release before publication; no allocation per IO buffer.
      auto preparation = makeUniqueNoThrow<Preparation>(journal, declarations, ioScratch);
      if (!preparation) return failure("OOM: preparation workspace");
      for (size_t i = 0; i < 2; ++i)
        if (pending[i] &&
            (!preparation->snapshot.prepareMatching(record, kind(i), match, context, snapshots[i]) || !guard()))
          return failure("snapshot preparation");
    }
    for (size_t i = 0; i < 2; ++i)
      if (!bind(i) || !publications[i]->publish() || !guard()) return failure("metadata publication");
    return true;
  }
  bool verify(const ContentRemovalRecord& record, const char* path) override {
    if (!select(record, path) ||
        (record.phase != ContentRemovalPhase::Quarantined && record.phase != ContentRemovalPhase::Committed))
      return failure("verify context");
    for (size_t i = 0; i < 2; ++i) {
      if (!load(i) || !bind(i)) return failure("verification declaration");
      const bool verified = record.phase == ContentRemovalPhase::Quarantined
                                ? publications[i]->verifyPublished()
                                : publications[i]->verifyRetiring(record, journal);
      if (!verified || !guard()) return failure("published metadata");
    }
    return true;
  }
  bool retire(const ContentRemovalRecord& record, const char* path) override {
    if (!select(record, path) || record.phase != ContentRemovalPhase::Committed || !verify(record, path))
      return failure("retire context");
    for (size_t i = 0; i < 2; ++i)
      if (!load(i) || !bind(i) || !publications[i]->retire(record, journal) || !guard())
        return failure("metadata retirement");
    return true;
  }
  bool verifyRetired(const ContentRemovalRecord& record, const char* path) override {
    if (!select(record, path) || record.phase < ContentRemovalPhase::Committed) return failure("retired context");
    for (size_t i = 0; i < 2; ++i)
      if (!load(i) || !bind(i) || !publications[i]->verifyRetired(record, journal) || !guard())
        return failure("retired metadata");
    return true;
  }

 private:
  struct Preparation {
    EpubReferenceJson json;
    HalEpubReferenceSnapshot snapshot;
    Preparation(ContentRemovalJournal& journal, HalRemovalMetadataSnapshotStorage& declarations,
                std::span<uint8_t> scratch)
        : snapshot(journal, declarations, json, scratch) {}
  };
  ContentRemovalJournal& journal;
  std::span<uint8_t> declarationScratch, ioScratch;
  HalRemovalMetadataSnapshotStorage declarations;
  HalRemovalMetadataAuthorization authorization;
  std::array<std::optional<HalRemovalMetadataPublisher>, 2> publications;
  std::array<RemovalMetadataSnapshot, 2> snapshots;
  std::array<bool, 2> pending{}, bound{};
  ContentRemovalRecord checkpoint;
  static RemovalMetadataFile kind(size_t i) { return i ? RemovalMetadataFile::Recent : RemovalMetadataFile::State; }
  bool select(const ContentRemovalRecord& record, const char* path) {
    if (!path || !validInventoryPath(path)) return failure("path arguments");
    return selectOwner(record);
  }
  bool selectOwner(const ContentRemovalRecord& record) {
    const auto a = reinterpret_cast<uintptr_t>(declarationScratch.data());
    const auto b = reinterpret_cast<uintptr_t>(ioScratch.data());
    const bool disjoint = a <= b ? declarationScratch.size() <= b - a : ioScratch.size() <= a - b;
    if (!validContentRemovalRecord(record) || record.request.manifest.kind != ContentKind::Epub ||
        declarationScratch.size() < REMOVAL_METADATA_SNAPSHOT_SIZE || ioScratch.empty() || !disjoint ||
        !journal.current() || *journal.current() != record)
      return failure("ownership/scratch");
    checkpoint = record;
    return true;
  }
  bool guard() const { return journal.current() && *journal.current() == checkpoint; }
  bool owned(size_t i) const {
    return snapshots[i].request == checkpoint.request && snapshots[i].planHash == checkpoint.planHash &&
           snapshots[i].file == kind(i);
  }
  bool load(size_t i) {
    return (declarations.load(checkpoint.planHash, kind(i), snapshots[i]) == RemovalMetadataStorageResult::Ok &&
            owned(i) && guard()) ||
           failure("load");
  }
  bool bind(size_t i) {
    if (!owned(i) || !guard() || !authorization.bind(snapshots[i])) return failure("authorization");
    if (!bound[i]) {
      if (!publications[i]->bind(snapshots[i])) return failure("publisher binding");
      bound[i] = true;
    }
    return guard();
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "EPUB reference removal failed: %s", reason);
    return false;
  }
};
}  // namespace companion

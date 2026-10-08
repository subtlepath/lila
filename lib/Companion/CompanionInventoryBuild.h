#pragma once
#include "CompanionInventoryPathsBuilder.h"
#include "CompanionInventoryRecovery.h"
#include "CompanionInventoryScan.h"
namespace companion {
class InventoryBuild {
 public:
  InventoryBuild(TransferStorage& storage, InventoryPublicationValidator& validator,
                 InventoryRevisionAllocator& revisions, InventoryRecovery& recovery, InventoryPublication& publication,
                 InventoryPathsBuilder& paths, InventoryPathsStage& pathStage, InventorySorter& sorter,
                 InventoryIndexBuilder& index, InventoryIndexSink& indexStage)
      : storage(storage),
        validator(validator),
        revisions(revisions),
        recovery(recovery),
        publication(publication),
        paths(paths),
        pathStage(pathStage),
        sorter(sorter),
        index(index),
        indexStage(indexStage) {}
  InventoryPublicationResult build(InventoryScan& scan, const Identity& generation, uint64_t& outputRevision) {
    if (!inventory_detail::nonzero(generation)) return InventoryPublicationResult::Invalid;
    const auto recovered = recovery.recover(generation);
    if (recovered != InventoryPublicationResult::Ok) return recovered;
    uint64_t bytes = 0, current = 0;
    const auto active = storage.stat(InventoryPublication::INDEX, bytes);
    if (active == FileStatus::Error) return InventoryPublicationResult::IoError;
    if (active == FileStatus::Present) {
      const auto checked =
          validator.pair(InventoryPublication::INDEX, InventoryPublication::PATHS, generation, 0, &current);
      if (checked != InventoryValidation::Valid)
        return checked == InventoryValidation::IoError ? InventoryPublicationResult::IoError
                                                       : InventoryPublicationResult::Corrupt;
    }
    if (active == FileStatus::Present && current == 0) return InventoryPublicationResult::Corrupt;
    uint64_t revision = 0;
    if (!revisions.reserve(current, revision)) return InventoryPublicationResult::IoError;
    if (revision == 0 || revision <= current) return InventoryPublicationResult::Corrupt;
    struct Guard {
      InventoryScan& scan;
      InventoryPathsBuilder& paths;
      InventoryPathsStage& pathStage;
      InventoryIndexSink& indexStage;
      bool closed = false, handedOff = false;
      ~Guard() {
        if (!closed) scan.closeScan();
        if (!handedOff) {
          paths.abort();
          pathStage.abort();
          indexStage.abort();
        }
      }
    } guard{scan, paths, pathStage, indexStage};
    if (!paths.begin(generation, revision) || !scan.beginScan()) return InventoryPublicationResult::IoError;
    if (!sorter.build(scan)) return InventoryPublicationResult::IoError;
    if (!scan.closeScan()) return InventoryPublicationResult::IoError;
    guard.closed = true;
    if (!paths.seal() || !index.build(sorter, generation, revision)) return InventoryPublicationResult::IoError;
    // Both sealed candidates now belong to publication/restart recovery.
    guard.handedOff = true;
    const auto result = publication.publish(generation, revision);
    if (result == InventoryPublicationResult::Ok) outputRevision = revision;
    return result;
  }

 private:
  TransferStorage& storage;
  InventoryPublicationValidator& validator;
  InventoryRevisionAllocator& revisions;
  InventoryRecovery& recovery;
  InventoryPublication& publication;
  InventoryPathsBuilder& paths;
  InventoryPathsStage& pathStage;
  InventorySorter& sorter;
  InventoryIndexBuilder& index;
  InventoryIndexSink& indexStage;
};
}  // namespace companion

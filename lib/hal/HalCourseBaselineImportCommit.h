#pragma once

#if LILA_TINTA

#include "CompanionWorkspace.h"
#include "HalCourseBaselineImportSession.h"
#include "HalCourseBaselineJournalReadiness.h"
#include "HalTransferStorage.h"

namespace companion {
// Caller authenticates identities and exclusively leases full scratch. Parent
// is the transfer's ordinary scratch loan within that full workspace.
inline TransferResult commitHalCourseBaselineImport(
    Transfer& transfer, HalTransferStorage& storage, const Identity& reader, const Identity& generation,
    const Identity& owner, const Identity& transaction, std::span<uint8_t> scratch, std::span<uint8_t> parentWorkspace,
    CourseBaselineSessionLimits limits, HalCourseBaselineLearnerInspection::Permission permitted, void* context) {
  if (!permitted || !permitted(context)) return TransferResult::Unauthorized;
  if (storage.hasCourseBaselineInstaller()) return TransferResult::Busy;
  const auto* current = transfer.current();
  if (!current) return TransferResult::NoTransaction;
  if (owner == Identity{} || current->owner != owner) return TransferResult::Unauthorized;
  if (generation == Identity{} || current->storageGeneration != generation) return TransferResult::WrongStorage;
  if (transaction == Identity{} || current->transaction != transaction ||
      transfer.destination() != COURSE_BASELINE_DESTINATION || !transfer.contentManifest())
    return TransferResult::Invalid;
  if (current->phase == TransferPhase::Aborted) return TransferResult::Invalid;
  if (current->phase == TransferPhase::Receiving && current->durableOffset != current->length)
    return TransferResult::Offset;
  if (reader == Identity{} || scratch.size() != SESSION_WORKSPACE_SIZE || !limits.depth || !limits.screens ||
      !limits.queued || parentWorkspace.size() != TRANSFER_SCRATCH_SIZE ||
      parentWorkspace.data() != scratch.data() + TRANSFER_OFFSET ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &transfer, sizeof(transfer)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &storage, sizeof(storage)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), reader.data(), reader.size()) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), generation.data(), generation.size()) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), owner.data(), owner.size()) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), transaction.data(), transaction.size())) {
    LOG_ERR("COMPANION", "Baseline commit workspace/identity loan refused");
    return TransferResult::Invalid;
  }
  if (!prepareHalCourseBaselineJournal(permitted, context)) return TransferResult::IoError;
  // Parser/inspection owners exceed the stack budget; factory admits their peak.
  auto session = createHalCourseBaselineImportSession(reader, generation, owner, scratch, limits, permitted, context,
                                                      parentWorkspace);
  if (!session) return TransferResult::IoError;
  storage.setCourseBaselineInstaller(session->installer());
  const auto result = transfer.commit(transaction, owner);
  storage.setCourseBaselineInstaller(nullptr);
  session.reset();
  return permitted(context) ? result : TransferResult::IoError;
}
}  // namespace companion

#endif

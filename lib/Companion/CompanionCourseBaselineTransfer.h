#pragma once

#include "CompanionCourseBaselineImportRequest.h"
#include "CompanionTransfer.h"

namespace companion {
inline constexpr char COURSE_BASELINE_DESTINATION[] = "/tinta/course-baseline.pack";
using CourseBaselineApproval = bool (*)(void*, const CourseBaselineImportRequest&, const TransferDeclaration&);
// Native callback saves/re-verifies exact consent with activities closed and all
// other writers excluded. It may reuse session scratch, but borrows inputs only.
// Separate native installation/recovery must validate and archive this payload.
[[gnu::noinline]] inline TransferResult beginCourseBaselineTransfer(
    [[maybe_unused]] Transfer& transfer, [[maybe_unused]] const CourseBaselineImportRequest& request,
    [[maybe_unused]] const Identity& generation, [[maybe_unused]] const Identity& owner,
    [[maybe_unused]] CourseBaselineApproval approve, [[maybe_unused]] void* context) {
#if !LILA_TINTA
  return TransferResult::Invalid;
#else
  if (!validCourseBaselineImportRequest(request)) return TransferResult::Invalid;
  if (owner == Identity{} || request.owner != owner) return TransferResult::Unauthorized;
  if (generation == Identity{} || request.generation != generation) return TransferResult::WrongStorage;
  if (!approve) return TransferResult::Unauthorized;
  const auto* current = transfer.current();
  if (current) {
    if (current->transaction == request.transaction) {
      const auto* manifest = transfer.contentManifest();
      if (current->owner != owner) return TransferResult::Unauthorized;
      if (!manifest || *manifest != request.manifest || current->storageGeneration != generation ||
          transfer.destination() != COURSE_BASELINE_DESTINATION)
        return TransferResult::Invalid;
    } else if (current->phase != TransferPhase::Committed && current->phase != TransferPhase::Aborted) {
      return TransferResult::Busy;
    }
  }
  // Freeze the declaration before approval performs any scratch-backed I/O.
  TransferDeclaration declaration;
  declaration.manifest = request.manifest;
  declaration.state.transaction = request.transaction;
  declaration.state.owner = request.owner;
  declaration.state.storageGeneration = request.generation;
  declaration.state.contentHash = request.manifest.contentHash;
  declaration.state.length = request.manifest.length;
  if (!approve(context, request, declaration)) return TransferResult::Unauthorized;
  return transfer.begin(declaration, COURSE_BASELINE_DESTINATION);
#endif
}
}  // namespace companion

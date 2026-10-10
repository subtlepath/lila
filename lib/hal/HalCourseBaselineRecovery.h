#pragma once

#if LILA_TINTA

#include <CompanionPairings.h>

#include "HalCourseBaselineImportSession.h"
#include "HalCourseBaselineJournalReadiness.h"
#include "HalTransferStorage.h"
#include "app/PersistedSessionLimits.h"

namespace companion {
// Caller has recovered journal publication/abort intents, excludes all writers,
// and lends the full workspace with transport queues/frame loans inactive.
// After attachment, normal transfer recovery is still mandatory.
inline bool attachHalCourseBaselineRecovery(Transfer& transfer, HalTransferStorage& storage,
                                            IdentityStorage& identityStorage, const IdentityState& identity,
                                            Pairings& pairings, std::span<uint8_t> fullWorkspace,
                                            std::span<uint8_t> parentWorkspace,
                                            std::unique_ptr<HalCourseBaselineImportSession>& session,
                                            HalCourseBaselineLearnerInspection::Permission permitted, void* context) {
  if (!permitted || !permitted(context) ||
      transfer.recover(identity.storageGeneration, TransferRecoveryMode::InspectJournal) != TransferResult::Ok) {
    LOG_ERR("COMPANION", "Baseline recovery transfer inspection or workspace permission failed");
    return false;
  }
  if (!transfer.current() || transfer.destination() != COURSE_BASELINE_DESTINATION) return true;
  IdentityState native;
  if (session || !transfer.contentManifest() || transfer.contentManifest()->kind != ContentKind::Course ||
      inspectIdentity(identityStorage, native) != IdentityInspectionResult::Ok || native.device != identity.device ||
      native.storageGeneration != identity.storageGeneration || pairings.load(fullWorkspace) != PairingResult::Ok ||
      !pairings.recognizes(transfer.current()->owner) || !prepareHalCourseBaselineJournal(permitted, context) ||
      !permitted(context)) {
    LOG_ERR("COMPANION", "Baseline recovery identity, owner or journal unavailable");
    return false;
  }
  session = createHalCourseBaselineImportSession(
      native.device, native.storageGeneration, transfer.current()->owner, fullWorkspace,
      {tinta::app::kSessionStackCapacity, static_cast<uint8_t>(tinta::app::ScreenId::Count),
       tinta::app::kSessionQueueCapacity},
      permitted, context, parentWorkspace);
  if (!session) return false;
  storage.setCourseBaselineInstaller(session->installer());
  return true;
}
}  // namespace companion

#endif

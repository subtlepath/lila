#pragma once
#include "CompanionFirmwareInstallIntent.h"

namespace companion {
// Caller authenticates owner and serializes storage/radio commands throughout admission.
inline FirmwareInstallIntentResult admitFirmwareInstallation(TransferStorage& storage,
                                                             const FirmwareInstallAuthorization& authorization,
                                                             const FirmwareReaderInfo& fresh,
                                                             const TransferState& state,
                                                             const ContentManifest& manifest,
                                                             std::string_view destination, std::span<uint8_t> scratch) {
  if (!validFirmwareInstallAuthorization(authorization) || scratch.size() < FIRMWARE_INSTALL_INTENT_SIZE ||
      !firmwareInstallCompatible(authorization.request, fresh) ||
      !firmwareInstallMatchesTransfer(authorization.request, authorization.owner, state, manifest, destination))
    return FirmwareInstallIntentResult::Invalid;
  if (!storage.verify(FIRMWARE_STAGE_DESTINATION, authorization.request.length, authorization.request.hash, scratch))
    return FirmwareInstallIntentResult::Corrupt;
  FirmwareInstallIntent intent(storage, scratch);
  return intent.persist(authorization);
}
}  // namespace companion

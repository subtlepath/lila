#pragma once

#include <Logging.h>

#include "CompanionTintaDerivedManifest.h"
#include "HalInventoryFileHash.h"

namespace companion {
enum class TintaDerivedVerification { Verified, Conflict, IoError };

// Caller excludes writers and keeps immutable manifest bytes separate from scratch.
inline TintaDerivedVerification verifyTintaDerivedFileReceipt(HalFile& file, const TintaDerivedManifestView& manifest,
                                                              TintaDerivedFile kind, std::span<uint8_t> scratch,
                                                              InventoryHashProgress progress = nullptr,
                                                              void* context = nullptr) {
  const auto expectedHash = manifest.hash(kind);
  if (expectedHash.size() != 32 || scratch.empty()) {
    LOG_ERR("COMPANION", "Invalid Tinta derived receipt verification input");
    return TintaDerivedVerification::Conflict;
  }
  if (!file.isOpen() || file.isDirectory()) {
    LOG_ERR("COMPANION", "Tinta derived receipt file unavailable");
    return TintaDerivedVerification::IoError;
  }
  const uint64_t expectedLength = manifest.length(kind);
  if (file.fileSize64() != expectedLength) {
    LOG_ERR("COMPANION", "Tinta derived receipt length mismatch");
    return TintaDerivedVerification::Conflict;
  }
  Digest actual{};
  uint64_t length = 0;
  if (!hashInventoryFile(file, scratch, length, actual, progress, context)) return TintaDerivedVerification::IoError;
  if (length != expectedLength || !std::equal(actual.begin(), actual.end(), expectedHash.begin())) {
    LOG_ERR("COMPANION", "Tinta derived receipt hash mismatch");
    return TintaDerivedVerification::Conflict;
  }
  return TintaDerivedVerification::Verified;
}
}  // namespace companion

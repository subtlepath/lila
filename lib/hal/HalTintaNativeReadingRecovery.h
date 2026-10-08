#pragma once

#include "CompanionTintaLegacyStoryIdentity.h"
#include "CompanionTintaNativeMarkSnapshot.h"
#include "HalTintaCompletionSetView.h"
#include "HalTintaDerivedFileVerification.h"

namespace companion {
// Caller verifies the immutable pack and completion receipt against the same course/frontier.
inline TintaNativeMarkSnapshotResult restoreTintaNativeReadings(tinta::core::StateStore& store,
                                                                HalTintaCompletionSetView& completions,
                                                                const tinta::core::pack::Pack& pack,
                                                                std::span<uint8_t> scratch) {
  if (!pack.isOpen()) {
    LOG_ERR("COMPANION", "Native reading recovery: pack unavailable");
    return TintaNativeMarkSnapshotResult::Invalid;
  }
  uint32_t count = 0;
  if (!completions.entryCount(TintaCompletionKind::Readings, count)) {
    LOG_ERR("COMPANION", "Native reading recovery: completion source unavailable");
    return TintaNativeMarkSnapshotResult::IoError;
  }
  struct Context {
    HalTintaCompletionSetView& completions;
    const tinta::core::pack::Pack& pack;
  } context{completions, pack};
  const auto identityAt = [](void* opaque, uint32_t index, uint32_t& identity) {
    return static_cast<Context*>(opaque)->completions.identityAt(index, identity);
  };
  const auto legacyKey = [](void* opaque, uint32_t identity, uint32_t& key) {
    return resolveTintaStableStoryIdentity(static_cast<Context*>(opaque)->pack, identity, key) ==
           LegacyStoryIdentityResult::Matched;
  };
  const auto result = restoreTintaNativeMarks(store, "read.bin", count, &context, identityAt, legacyKey, scratch);
  if (result != TintaNativeMarkSnapshotResult::Ok)
    LOG_ERR("COMPANION", "Native reading recovery failed: %u", static_cast<unsigned>(result));
  return result;
}
// Caller verifies the pack digest and excludes source writers throughout this operation.
inline TintaNativeMarkSnapshotResult restoreVerifiedTintaNativeReadings(tinta::core::StateStore& store, HalFile& file,
                                                                        const TintaDerivedManifestView& manifest,
                                                                        const Identity& course, const Identity& storage,
                                                                        const Digest& packHash, const Digest& frontier,
                                                                        const tinta::core::pack::Pack& pack,
                                                                        std::span<uint8_t> scratch) {
  if (!manifest.matches(course, storage, packHash, frontier) || !pack.isOpen() ||
      scratch.size() < TINTA_NATIVE_MARK_SNAPSHOT_SIZE) {
    LOG_ERR("COMPANION", "Native reading recovery: invalid receipt binding or workspace");
    return TintaNativeMarkSnapshotResult::Invalid;
  }
  const auto verified = verifyTintaDerivedFileReceipt(file, manifest, TintaDerivedFile::Readings, scratch);
  if (verified != TintaDerivedVerification::Verified)
    return verified == TintaDerivedVerification::Conflict ? TintaNativeMarkSnapshotResult::Invalid
                                                          : TintaNativeMarkSnapshotResult::IoError;
  HalTintaCompletionSetView completions(file);
  if (!completions.begin(TintaCompletionKind::Readings, scratch)) return TintaNativeMarkSnapshotResult::IoError;
  return restoreTintaNativeReadings(store, completions, pack, scratch);
}
}  // namespace companion

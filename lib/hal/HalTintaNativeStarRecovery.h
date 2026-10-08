#pragma once

#include "CompanionJournalCourseMembership.h"
#include "CompanionTintaNativeMarkSnapshot.h"
#include "HalTintaDerivedFileVerification.h"
#include "HalTintaItemSnapshotValidation.h"

namespace companion {
inline constexpr size_t TINTA_NATIVE_STAR_WORKSPACE_SIZE =
    TINTA_NATIVE_MARK_SNAPSHOT_SIZE + tinta::core::library::MarkLog::kCapacity * 4;
// Caller excludes source writers and binds the validated catalog to the receipt's pack.
inline TintaNativeMarkSnapshotResult restoreVerifiedTintaNativeStars(
    tinta::core::StateStore& store, HalFile& file, const TintaDerivedManifestView& manifest, const Identity& course,
    const Identity& storage, const Digest& packHash, const Digest& frontier, TintaSubjectCatalog& catalog,
    std::span<uint8_t> scratch, InventoryHashProgress progress = nullptr, void* progressContext = nullptr) {
  const auto failure = [](TintaNativeMarkSnapshotResult result) {
    LOG_ERR("COMPANION", "Native star recovery failed: %u", static_cast<unsigned>(result));
    return result;
  };
  if (!store.available() || !manifest.matches(course, storage, packHash, frontier) ||
      scratch.size() < TINTA_NATIVE_STAR_WORKSPACE_SIZE)
    return failure(TintaNativeMarkSnapshotResult::Invalid);
  const auto verified =
      verifyTintaDerivedFileReceipt(file, manifest, TintaDerivedFile::Items, scratch, progress, progressContext);
  if (verified != TintaDerivedVerification::Verified)
    return failure(verified == TintaDerivedVerification::Conflict ? TintaNativeMarkSnapshotResult::Invalid
                                                                  : TintaNativeMarkSnapshotResult::IoError);
  if (!validateTintaItemSnapshot(file, manifest.studyDay(), scratch, progress, progressContext) || !file.seek64(1024))
    return failure(TintaNativeMarkSnapshotResult::IoError);
  auto keys = scratch.subspan(TINTA_NATIVE_MARK_SNAPSHOT_SIZE, tinta::core::library::MarkLog::kCapacity * 4);
  uint32_t count = 0;
  const uint64_t length = file.fileSize64();
  if (length < 1024 || length != manifest.length(TintaDerivedFile::Items))
    return failure(TintaNativeMarkSnapshotResult::Invalid);
  const uint64_t records = (length - 1024) / 16;
  std::array<uint8_t, 16> bytes{};
  for (uint64_t at = 0; at < records; ++at) {
    if (at % 32 == 0 && progress && !progress(progressContext)) return failure(TintaNativeMarkSnapshotResult::IoError);
    tinta::core::ItemState item;
    if (file.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size()) ||
        !tinta::core::ItemState::decode(bytes.data(), item))
      return failure(TintaNativeMarkSnapshotResult::IoError);
    if (!(item.flags & tinta::core::item_flag::kStarred)) continue;
    if (count == tinta::core::library::MarkLog::kCapacity) return failure(TintaNativeMarkSnapshotResult::Invalid);
    const auto membership = catalog.contains(EventKind::Star, item.uid);
    if (membership != TintaSubjectMembership::Present)
      return failure(membership == TintaSubjectMembership::IoError ? TintaNativeMarkSnapshotResult::IoError
                                                                   : TintaNativeMarkSnapshotResult::Unresolved);
    binary_record::putU32(keys.data() + count++ * 4, item.uid);
  }
  struct Context {
    std::span<const uint8_t> keys;
    uint32_t count;
  } context{keys, count};
  const auto identityAt = [](void* opaque, uint32_t index, uint32_t& output) {
    const auto& owner = *static_cast<Context*>(opaque);
    if (index >= owner.count) return false;
    output = binary_record::getU32(owner.keys.data() + index * 4);
    return true;
  };
  const auto legacyKey = [](void*, uint32_t identity, uint32_t& output) {
    output = identity;
    return true;
  };
  if (progress && !progress(progressContext)) return failure(TintaNativeMarkSnapshotResult::IoError);
  const auto result = restoreTintaNativeMarks(store, "starred.bin", count, &context, identityAt, legacyKey,
                                              scratch.first(TINTA_NATIVE_MARK_SNAPSHOT_SIZE));
  return result == TintaNativeMarkSnapshotResult::Ok ? result : failure(result);
}
}  // namespace companion

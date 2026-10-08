#pragma once

#include "HalTintaCompletionSetView.h"
#include "HalTintaDayLogValidation.h"
#include "HalTintaDerivedFileVerification.h"
#include "HalTintaItemSnapshotValidation.h"

namespace companion {
// Caller retains immutable manifest bytes and excludes writes until publication finishes.
class HalTintaDerivedGenerationValidation {
 public:
  bool begin(const TintaDerivedManifestView& candidate, const Identity& course, const Identity& storage,
             const Digest& pack, const Digest& frontier) {
    ready = false;
    verified = 0;
    if (!candidate.matches(course, storage, pack, frontier)) return failure("manifest binding");
    manifest = candidate;
    ready = true;
    return true;
  }
  bool file(TintaDerivedFile kind, HalFile& file, std::span<uint8_t> scratch, InventoryHashProgress progress = nullptr,
            void* context = nullptr) {
    if (!ready || scratch.size() < 80) return failure("state or scratch");
    if (verifyTintaDerivedFileReceipt(file, manifest, kind, scratch, progress, context) !=
        TintaDerivedVerification::Verified)
      return failure("receipt");
    bool valid = false;
    switch (kind) {
      case TintaDerivedFile::Items:
        valid = validateTintaItemSnapshot(file, manifest.studyDay(), scratch, progress, context);
        break;
      case TintaDerivedFile::LocalReviews:
        valid = true;
        break;
      case TintaDerivedFile::Lessons:
      case TintaDerivedFile::Readings: {
        HalTintaCompletionSetView view(file);
        valid =
            view.begin(kind == TintaDerivedFile::Lessons ? TintaCompletionKind::Lessons : TintaCompletionKind::Readings,
                       scratch, progress, context);
        break;
      }
      case TintaDerivedFile::Days:
        valid = validateTintaDayLogFile(file, scratch, progress, context);
        break;
    }
    if (!valid) return failure("format");
    verified |= static_cast<uint8_t>(1u << static_cast<uint8_t>(kind));
    return true;
  }
  bool complete() const { return ready && verified == 31; }

 private:
  bool failure(const char* reason) {
    ready = false;
    verified = 0;
    LOG_ERR("COMPANION", "Tinta derived generation validation failed: %s", reason);
    return false;
  }
  TintaDerivedManifestView manifest;
  uint8_t verified = 0;
  bool ready = false;
};
}  // namespace companion

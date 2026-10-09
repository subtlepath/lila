#pragma once

#include <Memory.h>

#include "CompanionBookmarkLegacyAnchor.h"
#include "CompanionBookmarkMigrationClaim.h"
#include "CompanionBookmarkPreparationClaim.h"
#include "CompanionIdentity.h"

namespace companion {
// Stable batch context. Caller freezes the verified EPUB/path and all writers.
class NativeBookmarkReaderContext final {
 public:
  NativeBookmarkReaderContext(Epub& epub, IdentityStorage& identities, const Digest& edition,
                              const IdentityState& identity)
      : epub(epub),
        identities(identities),
        edition(edition),
        generation(identity.storageGeneration),
        device(identity.device) {}
  NativeBookmarkReaderContext(const NativeBookmarkReaderContext&) = delete;
  NativeBookmarkReaderContext& operator=(const NativeBookmarkReaderContext&) = delete;
  bool validate(const Digest& candidateEdition, const Identity& candidateGeneration) {
    if (!tinta_body_detail::nonzero(edition) || !tinta_body_detail::nonzero(generation) ||
        candidateEdition != edition || candidateGeneration != generation || !epub.hasCompanionContentIdentity() ||
        !epub.getCompanionContentIdentity({}, observedEdition) || observedEdition != edition ||
        inspectIdentity(identities, observedIdentity) != IdentityInspectionResult::Ok ||
        observedIdentity.device != device || observedIdentity.storageGeneration != generation) {
      LOG_ERR("COMPANION", "Bookmark reader context changed or unavailable");
      return false;
    }
    return true;
  }
  static bool migration(void* opaque, const BookmarkMigrationClaim& claim) {
    auto& self = *static_cast<NativeBookmarkReaderContext*>(opaque);
    return self.validate(claim.edition, claim.storageGeneration);
  }
  static bool preparation(void* opaque, const BookmarkPreparationClaim& claim) {
    auto& self = *static_cast<NativeBookmarkReaderContext*>(opaque);
    return self.validate(claim.edition, claim.storageGeneration);
  }
  static bool publication(void* opaque, const BookmarkPublicationClaim& claim) {
    auto& self = *static_cast<NativeBookmarkReaderContext*>(opaque);
    return self.validate(claim.edition, claim.storageGeneration);
  }
  static bool resolveLegacy(void* opaque, BookmarkEntry& entry) {
    auto& self = *static_cast<NativeBookmarkReaderContext*>(opaque);
    if (!self.validate(self.edition, self.generation)) return false;
    if (entry.hasVisibleTextOffset) return entry.computedSpineIndex < self.epub.getSpineItemsCount();
    // XML pools/traversal exceed the task-local limit; reuse one checked owner.
    if (!self.anchors) {
      self.anchors = makeUniqueNoThrow<NativeBookmarkLegacyAnchor>();
      if (!self.anchors) {
        LOG_ERR("COMPANION", "OOM: bookmark legacy resolver");
        return false;
      }
    }
    const auto previousSpine = entry.computedSpineIndex;
    const auto previousOffset = entry.visibleTextOffset;
    if (!self.anchors->resolve(self.epub, entry)) return false;
    if (self.validate(self.edition, self.generation)) return true;
    entry.computedSpineIndex = previousSpine;
    entry.visibleTextOffset = previousOffset;
    entry.hasVisibleTextOffset = false;
    return false;
  }
  static bool current(void* opaque) {
    auto& self = *static_cast<NativeBookmarkReaderContext*>(opaque);
    return self.validate(self.edition, self.generation);
  }
  static void release(void* opaque) { static_cast<NativeBookmarkReaderContext*>(opaque)->releaseLegacyResolver(); }
  void releaseLegacyResolver() { anchors.reset(); }

 private:
  Epub& epub;
  IdentityStorage& identities;
  Digest edition{}, observedEdition{};
  Identity generation{}, device{};
  IdentityState observedIdentity{};
  std::unique_ptr<NativeBookmarkLegacyAnchor> anchors;
};
}  // namespace companion

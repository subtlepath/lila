#pragma once

#include "HalTintaReplayExport.h"
#include "HalTintaReplaySession.h"

namespace companion {
// Checked, session-owned workspace. Caller freezes writers and binds the immutable
// validated catalog to this course/pack and the current reader storage generation.
class HalTintaDerivedJournalProof {
 public:
  HalTintaDerivedJournalProof(const Identity& course, const Identity& storage, const Digest& pack,
                              TintaSubjectCatalog& catalog, std::span<uint8_t> scratch)
      : course(course), storage(storage), pack(pack), catalog(catalog), scratch(scratch) {}
  bool prove(const TintaDerivedManifestView& manifest) {
    if (scratch.size() < 80 || !replay.run(course, catalog)) return failure("journal replay");
    const auto* frontier = replay.journalFrontier();
    auto* store = replay.workingStore();
    if (!frontier || !store || !manifest.matches(course, storage, pack, *frontier))
      return failure("frontier or binding");
    if (!output.run(*store, course, manifest.studyDay(), scratch)) return failure("proof export");
    for (unsigned at = 0; at < 5; ++at) {
      uint64_t length = 0;
      Digest hash{};
      const auto kind = static_cast<TintaDerivedFile>(at);
      if (!output.receipt(kind, length, hash) || length != manifest.length(kind) ||
          !std::equal(hash.begin(), hash.end(), manifest.hash(kind).begin()))
        return failure("derived receipt");
    }
    return true;
  }
  static bool callback(void* context, const TintaDerivedManifestView& manifest) {
    if (!context) return failure("missing context");
    return static_cast<HalTintaDerivedJournalProof*>(context)->prove(manifest);
  }

 private:
  static bool failure(const char* stage) {
    LOG_ERR("COMPANION", "Tinta journal proof %s failed", stage);
    return false;
  }
  Identity course, storage;
  Digest pack;
  TintaSubjectCatalog& catalog;
  std::span<uint8_t> scratch;
  HalTintaReplaySession replay;
  HalTintaReplayExport output{TintaReplayExportTarget::Proof};
};
}  // namespace companion

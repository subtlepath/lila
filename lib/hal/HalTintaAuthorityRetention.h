#pragma once

#include "CompanionTintaDerivedManifest.h"
#include "HalJournalCausalAuditSession.h"
#include "HalTintaAuthorityCheckpoints.h"

namespace companion {
// Retain off-stack; caller validates the installed catalog and freezes authority
// writers throughout checkpoint creation, retention proof, and publication.
class HalTintaAuthorityRetention final {
 public:
  bool establish(std::span<const uint8_t> manifestBytes, const Identity& course, const Identity& generation,
                 const Digest& pack, TintaSubjectCatalog& catalog) {
    if (!prepare(manifestBytes, course, generation, pack)) return failure("manifest binding");
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit) return failure("OOM: checkpoint audit workspace");
    if (!audit->run(&current, &course, &catalog) || current != baseline) return failure("baseline authority");
    checkpoint.manifest = manifestDigest;
    checkpoint.frontier = baseline;
    checkpoint.count = audit->recordCount();
    checkpoint.recordSize = audit->recordSize();
    audit.reset();
    return records.persist(checkpoint) == TintaAuthorityCheckpointResult::Ok;
  }
  bool prove(std::span<const uint8_t> manifestBytes, const Identity& course, const Identity& generation,
             const Digest& pack, TintaSubjectCatalog& catalog, const Digest& expectedCurrent) {
    if (!prepare(manifestBytes, course, generation, pack) ||
        records.load(manifestDigest, checkpoint) != TintaAuthorityCheckpointResult::Ok)
      return failure("checkpoint binding");
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit) return failure("OOM: retention audit workspace");
    if (!audit->run(&current, &course, &catalog) || current != expectedCurrent ||
        !audit->prefixFrontier(checkpoint.count, prefix))
      return failure("current authority or prefix");
    return verifyTintaAuthorityCheckpoint(checkpoint, manifestDigest, baseline, audit->recordCount(),
                                          audit->recordSize(), prefix) ||
           failure("retained authority");
  }

 private:
  bool prepare(std::span<const uint8_t> bytes, const Identity& course, const Identity& generation, const Digest& pack) {
    if (!manifest.decode(bytes)) return false;
    std::copy_n(bytes.begin() + 52, baseline.size(), baseline.begin());
    return manifest.matches(course, generation, pack, baseline) && hashing.digest(bytes, manifestDigest);
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Tinta authority retention failed: %s", reason);
    return false;
  }
  HalTintaAuthorityCheckpoints records;
  HalTintaJournalStorage hashing;
  TintaDerivedManifestView manifest;
  TintaAuthorityCheckpoint checkpoint;
  Digest manifestDigest{}, baseline{}, current{}, prefix{};
};
}  // namespace companion

#pragma once

#include "HalTintaProvenPublication.h"

namespace companion {
enum class TintaIncrementalRecoveryResult { Unchanged, Rebuilt, NoReceipt, Failed };
// Checked off-stack startup workspace. Caller recovers pending publication first,
// validates installed pack/catalog, freezes writers, and supplies a fresh snapshot ID.
class HalTintaIncrementalRecovery final {
 public:
  using RetentionProof = bool (*)(void*, const TintaDerivedManifestView&, const Digest&);
  HalTintaIncrementalRecovery(const Identity& course, void* retentionContext = nullptr,
                              RetentionProof proveRetention = nullptr)
      : course(course), retentionContext(retentionContext), proveRetention(proveRetention), reader(course, scratch) {}
  TintaIncrementalRecoveryResult run(const Identity& generation, const Digest& pack, TintaSubjectCatalog& catalog,
                                     uint16_t knownDay, const Identity& snapshot) {
    ready = false;
    if (reader.load(TintaDerivedRecord::Intent, nextBytes) != TintaDerivedRecordLoad::Missing)
      return failure("pending publication");
    const auto loaded = reader.load(TintaDerivedRecord::Receipt, previousBytes);
    if (loaded == TintaDerivedRecordLoad::Missing) return TintaIncrementalRecoveryResult::NoReceipt;
    if (loaded != TintaDerivedRecordLoad::Loaded || !previous.decode(previousBytes)) return failure("baseline receipt");
    std::copy_n(previousBytes.begin() + 52, previousFrontier.size(), previousFrontier.begin());
    if (!previous.matches(course, generation, pack, previousFrontier) || !tinta_body_detail::nonzero(snapshot))
      return failure("baseline binding");
    studyDay = std::max(knownDay, previous.studyDay());
    // Audit/replay/export owners retain fixed scratch and handles beyond the task
    // stack. Release each phase before allocating the next publication workspace.
    auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
    if (!audit) return failure("OOM: audit workspace");
    if (!audit->run(&frontier, &course, &catalog) || !audit->replay(this, &rememberDay))
      return failure("journal audit");
    audit.reset();
    auto retention = makeUniqueNoThrow<HalTintaAuthorityRetention>();
    if (!retention) return failure("OOM: authority retention workspace");
    if (frontier == previousFrontier) {
      const auto verified = reader.verifyGeneration(previous);
      if (verified == TintaDerivedVerification::IoError) return failure("baseline file verification");
      if (!retention->establish(previousBytes, course, generation, pack, catalog))
        return failure("baseline checkpoint");
      if (verified == TintaDerivedVerification::Verified) {
        ready = true;
        return TintaIncrementalRecoveryResult::Unchanged;
      }
    }
    if (!retention->prove(previousBytes, course, generation, pack, catalog, frontier) ||
        (proveRetention && !proveRetention(retentionContext, previous, frontier)))
      return failure("authority retention");
    retention.reset();
    if (previous.revision() == UINT64_MAX ||
        std::equal(snapshot.begin(), snapshot.end(), previous.snapshotIdentity().begin()))
      return failure("snapshot identity or revision");
    auto replay = makeUniqueNoThrow<HalTintaReplaySession>();
    auto output = makeUniqueNoThrow<HalTintaReplayExport>();
    if (!replay || !output) return failure("OOM: replay/export workspace");
    if (!replay->run(course, catalog) || !replay->journalFrontier() || *replay->journalFrontier() != frontier ||
        !output->run(*replay->workingStore(), course, studyDay, scratch) ||
        !output->manifest(generation, pack, frontier, snapshot, previous.revision() + 1, nextBytes) ||
        !replay->workingStore()->close())
      return failure("replay/export");
    output.reset();
    replay.reset();
    if (publishProvenTintaDerived(course, generation, pack, catalog, nextBytes, scratch, previousBytes) !=
        TintaPublicationResult::Ok)
      return failure("publication");
    ready = true;
    return TintaIncrementalRecoveryResult::Rebuilt;
  }
  // First alpha baseline: old native caches may be replaced, but existing
  // authoritative course history requires its own recovery path.
  bool initialize(const Identity& generation, const Digest& pack, TintaSubjectCatalog& catalog,
                  const Identity& snapshot) {
    ready = false;
    if (!tinta_body_detail::nonzero(course) || !tinta_body_detail::nonzero(generation) ||
        !tinta_body_detail::nonzero(pack) || !tinta_body_detail::nonzero(snapshot) ||
        reader.load(TintaDerivedRecord::Intent, nextBytes) != TintaDerivedRecordLoad::Missing ||
        reader.load(TintaDerivedRecord::Receipt, previousBytes) != TintaDerivedRecordLoad::Missing)
      return initializationFailure("arguments or existing publication");
    {
      auto audit = makeUniqueNoThrow<HalJournalCausalAuditSession>();
      if (!audit) return initializationFailure("OOM: audit workspace");
      bool found = false;
      if (!audit->run(&frontier, &course, &catalog) || !audit->containsTintaCourse(course, found) || found)
        return initializationFailure("existing course authority");
    }
    auto replay = makeUniqueNoThrow<HalTintaReplaySession>();
    auto output = makeUniqueNoThrow<HalTintaReplayExport>();
    if (!replay || !output) return initializationFailure("OOM: replay/export workspace");
    if (!replay->run(course, catalog) || !replay->journalFrontier() || *replay->journalFrontier() != frontier ||
        !output->run(*replay->workingStore(), course, 0, scratch) ||
        !output->manifest(generation, pack, frontier, snapshot, 1, nextBytes) || !replay->workingStore()->close())
      return initializationFailure("initial replay/export");
    output.reset();
    replay.reset();
    if (publishProvenTintaDerived(course, generation, pack, catalog, nextBytes, scratch) != TintaPublicationResult::Ok)
      return initializationFailure("initial publication");
    ready = true;
    return true;
  }
  // Audited frontier only: caller must still run native derived preparation.
  const Digest* journalFrontier() const { return ready ? &frontier : nullptr; }

 private:
  static bool initializationFailure(const char* reason) {
    failure(reason);
    return false;
  }
  static bool rememberDay(void* context, uint32_t, const SyncEvent& event, std::span<const uint8_t> bytes, bool) {
    auto& owner = *static_cast<HalTintaIncrementalRecovery*>(context);
    if (event.kind < EventKind::Review) return true;
    if (!decodeTintaBody(bytes, owner.body)) return false;
    if (owner.body.course == owner.course)
      owner.studyDay = std::max(owner.studyDay, static_cast<uint16_t>(event.studyDay));
    return true;
  }
  static TintaIncrementalRecoveryResult failure(const char* reason) {
    LOG_ERR("COMPANION", "Tinta incremental recovery failed: %s", reason);
    return TintaIncrementalRecoveryResult::Failed;
  }
  Identity course;
  void* retentionContext;
  RetentionProof proveRetention;
  std::array<uint8_t, 512> scratch{};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> previousBytes{}, nextBytes{};
  HalTintaDerivedRecordReader reader;
  TintaDerivedManifestView previous;
  Digest previousFrontier{}, frontier{};
  TintaBody body;
  uint16_t studyDay = 0;
  bool ready = false;
};
}  // namespace companion

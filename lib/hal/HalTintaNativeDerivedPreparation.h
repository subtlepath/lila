#pragma once

#include "HalTintaDerivedGenerationValidation.h"
#include "HalTintaDerivedRecordReader.h"
#include "HalTintaNativeLessonRecovery.h"
#include "HalTintaNativeReadingRecovery.h"
#include "HalTintaNativeStarRecovery.h"

namespace companion {
enum class TintaNativePreparationResult { Prepared, NoReceipt, Failed };
// Checked off-stack startup workspace. Caller verifies installed pack/catalog and
// authoritative frontier, freezes writers, and supplies an already loaded profile.
class HalTintaNativeDerivedPreparation final {
 public:
  using JournalProof = bool (*)(void*, const TintaDerivedManifestView&);
  HalTintaNativeDerivedPreparation(const Identity& course, void* proofContext, JournalProof prove)
      : course(course), proofContext(proofContext), prove(prove), reader(course, scratch) {}
  ~HalTintaNativeDerivedPreparation() { close(); }
  TintaNativePreparationResult run(tinta::core::StateStore& store, tinta::core::Profile& profile,
                                   const tinta::core::pack::Pack& pack, TintaSubjectCatalog& catalog,
                                   const Identity& generation, const Digest& packHash, const Digest& frontier) {
    if (!close() || !store.available() || !pack.isOpen()) return failure("startup state");
    const auto loaded = reader.load(TintaDerivedRecord::Receipt, receipt);
    if (loaded == TintaDerivedRecordLoad::Missing) return TintaNativePreparationResult::NoReceipt;
    if (loaded != TintaDerivedRecordLoad::Loaded || !manifest.decode(receipt) ||
        !validation.begin(manifest, course, generation, packHash, frontier))
      return failure("receipt binding");
    if (!prove || !prove(proofContext, manifest)) return failure("journal proof");
    for (unsigned at = 0; at < 5; ++at) {
      const auto kind = static_cast<TintaDerivedFile>(at);
      const bool valid = open(kind) && validation.file(kind, file, scratch);
      const bool closed = close();
      if (!valid || !closed) return failure("generation preflight");
    }
    if (!validation.complete()) return failure("incomplete generation");
    if (!open(TintaDerivedFile::Readings)) return failure("readings open");
    const auto readings = restoreVerifiedTintaNativeReadings(store, file, manifest, course, generation, packHash,
                                                             frontier, pack, scratch);
    if (!close() || readings != TintaNativeMarkSnapshotResult::Ok) return failure("readings restore");
    if (!open(TintaDerivedFile::Items)) return failure("items open");
    const auto stars = restoreVerifiedTintaNativeStars(store, file, manifest, course, generation, packHash, frontier,
                                                       catalog, scratch);
    if (!close() || stars != TintaNativeMarkSnapshotResult::Ok) return failure("stars restore");
    if (!open(TintaDerivedFile::Lessons)) return failure("lessons open");
    const bool lessons = persistVerifiedTintaNativeLessons(store, profile, file, manifest, course, generation, packHash,
                                                           frontier, pack, scratch);
    if (!close() || !lessons) return failure("lessons restore");
    return TintaNativePreparationResult::Prepared;
  }

 private:
  bool open(TintaDerivedFile kind) {
    return close() && tintaDerivedFilePath(course, kind, TintaDerivedRole::Active, path) &&
           Storage.openFileForReadReusing("COMPANION", path.data(), file);
  }
  bool close() {
    if (!file.isOpen() || file.close()) return true;
    failure("file close");
    return false;
  }
  static TintaNativePreparationResult failure(const char* reason) {
    LOG_ERR("COMPANION", "Native derived preparation failed: %s", reason);
    return TintaNativePreparationResult::Failed;
  }
  Identity course;
  void* proofContext;
  JournalProof prove;
  std::array<uint8_t, TINTA_NATIVE_STAR_WORKSPACE_SIZE> scratch{};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> receipt{};
  HalTintaDerivedRecordReader reader;
  HalTintaDerivedGenerationValidation validation;
  TintaDerivedManifestView manifest;
  std::array<char, 96> path{};
  HalFile file;
};
}  // namespace companion

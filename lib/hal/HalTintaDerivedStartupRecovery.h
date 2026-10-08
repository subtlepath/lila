#pragma once

#if LILA_TINTA
#include <Memory.h>

#include "CompanionCourseBinding.h"
#include "CompanionCourseSource.h"
#include "CompanionCourseValidation.h"
#include "CompanionTintaPackSubjectCatalog.h"
#include "HalIdentityStorage.h"
#include "HalTintaProvenPublication.h"
#include "HalTransferStorage.h"

namespace companion {
// Short-lived startup workspace, released before allocating the learner UI.
class HalTintaDerivedStartupRecovery {
 public:
  HalTintaDerivedStartupRecovery(const Identity& course, IdentityStorage& identities)
      : course(course), reader(course, scratch), identities(identities), source(packStorage), catalog(pack, source) {}
  ~HalTintaDerivedStartupRecovery() {
    pack.close();
    if (!packStorage.close()) failure("pack close");
  }
  bool run() {
    const auto pending = reader.load(TintaDerivedRecord::Intent, pendingBytes);
    if (pending == TintaDerivedRecordLoad::Missing) return true;
    if (pending != TintaDerivedRecordLoad::Loaded) return failure("pending manifest");
    bool present = false;
    if (readCourseBinding(transfer, COURSE_BINDING_PATH, scratch, installed, present) != CourseBindingResult::Ok ||
        !present || installed.logicalIdentity != course ||
        !transfer.verify(ACTIVE_COURSE_PATH, installed.length, installed.contentHash, scratch))
      return failure("installed course binding");
    if (provisionIdentity(identities, identity) != IdentityResult::Ok) return failure("storage identity");
    TintaDerivedManifestView manifest;
    if (!manifest.decode(pendingBytes)) return failure("manifest decode");
    std::copy_n(pendingBytes.begin() + 52, frontier.size(), frontier.begin());
    if (!manifest.matches(course, identity.storageGeneration, installed.contentHash, frontier))
      return failure("manifest binding");
    if (!packStorage.open(ACTIVE_COURSE_PATH) || !source.attach() ||
        validateCourseCandidate(pack, source, scratch) != CourseValidationResult::Ok || !catalog.prepare(scratch))
      return failure("course validation");
    const auto receipt = reader.load(TintaDerivedRecord::Receipt, previousBytes);
    if (receipt != TintaDerivedRecordLoad::Loaded && receipt != TintaDerivedRecordLoad::Missing)
      return failure("previous receipt");
    const auto previous = receipt == TintaDerivedRecordLoad::Loaded ? std::span<const uint8_t>(previousBytes)
                                                                    : std::span<const uint8_t>{};
    const auto result = publishProvenTintaDerived(course, identity.storageGeneration, installed.contentHash, catalog,
                                                  pendingBytes, scratch, previous);
    return result == TintaPublicationResult::Ok || failure("publication recovery");
  }

 private:
  static bool failure(const char* stage) {
    LOG_ERR("COMPANION", "Tinta startup publication %s failed", stage);
    return false;
  }
  Identity course;
  std::array<uint8_t, 512> scratch{};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> pendingBytes{}, previousBytes{};
  HalTintaDerivedRecordReader reader;
  HalTransferStorage transfer;
  IdentityStorage& identities;
  IdentityState identity;
  ContentManifest installed;
  Digest frontier{};
  HalInventoryIndexStorage packStorage;
  StoredCourseSource source;
  tinta::core::pack::Pack pack;
  TintaPackSubjectCatalog catalog;
};
inline bool recoverBoundTintaDerivedPublication(const Identity& course) {
  // Retained paths, manifests and Pack exceed the task-local budget.
  HalIdentityStorage identities;
  auto recovery = makeUniqueNoThrow<HalTintaDerivedStartupRecovery>(course, identities);
  if (!recovery) {
    LOG_ERR("COMPANION", "OOM: Tinta startup publication workspace");
    return false;
  }
  return recovery->run();
}
}  // namespace companion
#endif

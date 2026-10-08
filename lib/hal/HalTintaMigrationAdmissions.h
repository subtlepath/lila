#pragma once

#include "CompanionTintaJournal.h"
#include "CompanionTintaMigrationAdmissionPaths.h"
#include "HalTintaMigrationAdmissionStore.h"

namespace companion {
// Checked off-stack owner; hashing storage outlives it. Caller verifies migration
// admission and excludes record/journal writers throughout publication.
class HalTintaMigrationAdmissions final {
 public:
  explicit HalTintaMigrationAdmissions(TintaJournalStorage& hashing)
      : hashing(hashing), records(target.data(), stage.data()) {}
  TintaMigrationAdmissionResult persist(const TintaMigrationAdmission& value) {
    if (!validTintaMigrationAdmission(value)) return TintaMigrationAdmissionResult::Invalid;
    const auto result = prepare(value.course, value.merge);
    return result == TintaMigrationAdmissionResult::Ok ? records.persist(value) : result;
  }
  TintaMigrationAdmissionResult load(const Identity& course, const JournalMergeIntent& merge,
                                     TintaMigrationAdmission& output) {
    auto result = prepare(course, merge);
    if (result != TintaMigrationAdmissionResult::Ok) return result;
    result = records.load(loaded);
    if (result != TintaMigrationAdmissionResult::Ok) return result;
    if (loaded.course != course || loaded.merge != merge) return TintaMigrationAdmissionResult::Conflict;
    output = loaded;
    return result;
  }

 private:
  TintaMigrationAdmissionResult prepare(const Identity& course, const JournalMergeIntent& merge) {
    if (!encodeTintaMigrationAddress(course, merge, address)) return TintaMigrationAdmissionResult::Invalid;
    if (!hashing.digest(address, digest)) {
      LOG_ERR("COMPANION", "Tinta migration address digest failed");
      return TintaMigrationAdmissionResult::IoError;
    }
    if (!tintaMigrationAdmissionPath(digest, false, target) || !tintaMigrationAdmissionPath(digest, true, stage))
      return TintaMigrationAdmissionResult::Invalid;
    return TintaMigrationAdmissionResult::Ok;
  }
  TintaJournalStorage& hashing;
  std::array<uint8_t, TINTA_MIGRATION_ADDRESS_SIZE> address{};
  Digest digest{};
  std::array<char, TINTA_MIGRATION_PATH_SIZE> target{}, stage{};
  HalTintaMigrationAdmissionStore records;
  TintaMigrationAdmission loaded;
};
}  // namespace companion

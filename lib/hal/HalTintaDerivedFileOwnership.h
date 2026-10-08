#pragma once

#include "HalTintaDerivedFileVerification.h"
#include "HalTintaDerivedRecordReader.h"

namespace companion {
// Session owns fixed paths; caller keeps manifest immutable and excludes all other writers.
class HalTintaDerivedFileOwnership {
 public:
  HalTintaDerivedFileOwnership(const Identity& course, std::span<const uint8_t> expected,
                               HalTintaDerivedRecordReader& records, std::span<uint8_t> scratch)
      : course(course), expected(expected), records(records), scratch(scratch), lookup(nullptr, nullptr, root.data()) {
    ready = scratch.size() >= 80 && courseStateDirectory(course, root) && manifest.decode(expected) &&
            std::equal(course.begin(), course.end(), expected.begin() + 4);
  }
  ~HalTintaDerivedFileOwnership() {
    if (file.isOpen() && !file.close()) LOG_ERR("COMPANION", "Tinta file ownership close failed");
  }
  TintaPublicationState state(TintaDerivedFile kind, TintaDerivedRole role) {
    if (!ready || !tintaDerivedFilePath(course, kind, role, source) || (file.isOpen() && !file.close()))
      return error("state arguments or close");
    const auto presence = lookup.inspect(source.data());
    if (presence == CompanionFilePresence::Missing) return TintaPublicationState::Missing;
    if (presence == CompanionFilePresence::Error) return error("lookup");
    if (!Storage.openFileForReadReusing("COMPANION", source.data(), file)) return error("open");
    if (file.isDirectory()) {
      if (!file.close()) return error("directory close");
      return error("directory conflict");
    }
    const auto result = verifyTintaDerivedFileReceipt(file, manifest, kind, scratch);
    if (!file.close()) return error("close");
    if (result == TintaDerivedVerification::Verified) return TintaPublicationState::Matches;
    if (result == TintaDerivedVerification::Conflict) return TintaPublicationState::Other;
    return error("receipt read");
  }
  bool rename(TintaDerivedFile kind, TintaDerivedRole from, TintaDerivedRole to) {
    const bool backup = from == TintaDerivedRole::Active && to == TintaDerivedRole::Backup;
    const bool install = from == TintaDerivedRole::Candidate && to == TintaDerivedRole::Active;
    if ((!backup && !install) ||
        records.inspect(TintaDerivedRecord::Intent, expected) != TintaPublicationState::Matches)
      return failed("rename authorization");
    const auto current = state(kind, from);
    if (current != (backup ? TintaPublicationState::Other : TintaPublicationState::Matches) ||
        state(kind, to) != TintaPublicationState::Missing || !tintaDerivedFilePath(course, kind, from, source) ||
        !tintaDerivedFilePath(course, kind, to, destination))
      return failed("rename ownership");
    return Storage.rename(source.data(), destination.data()) || failed("rename");
  }
  bool remove(TintaDerivedFile kind, TintaDerivedRole role) {
    if ((role != TintaDerivedRole::Backup && role != TintaDerivedRole::Candidate) ||
        records.inspect(TintaDerivedRecord::Intent, expected) != TintaPublicationState::Matches)
      return failed("remove authorization");
    if (role == TintaDerivedRole::Backup &&
        records.inspect(TintaDerivedRecord::Receipt, expected) != TintaPublicationState::Matches)
      return failed("backup commit receipt");
    const auto current = state(kind, role);
    if (current == TintaPublicationState::Missing) return true;
    if (current == TintaPublicationState::Error) return false;
    if (role == TintaDerivedRole::Candidate &&
        (current != TintaPublicationState::Matches ||
         state(kind, TintaDerivedRole::Active) != TintaPublicationState::Matches))
      return failed("candidate ownership");
    if (!tintaDerivedFilePath(course, kind, role, source)) return failed("remove path");
    return Storage.remove(source.data()) || failed("remove");
  }

 private:
  static TintaPublicationState error(const char* reason) {
    LOG_ERR("COMPANION", "Tinta derived file %s failed", reason);
    return TintaPublicationState::Error;
  }
  static bool failed(const char* reason) {
    error(reason);
    return false;
  }
  Identity course;
  std::span<const uint8_t> expected;
  HalTintaDerivedRecordReader& records;
  std::span<uint8_t> scratch;
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> source{}, destination{};
  HalCompanionFileLookup lookup;
  HalFile file;
  TintaDerivedManifestView manifest;
  bool ready = false;
};
}  // namespace companion

#pragma once

#include "CompanionTintaDerivedPaths.h"
#include "HalCompanionFileLookup.h"

namespace companion {
enum class TintaDerivedRecordLoad { Loaded, Missing, Invalid, IoError };
// Session-owned handles; caller retains scratch separate from immutable expected manifest bytes.
class HalTintaDerivedRecordReader {
 public:
  HalTintaDerivedRecordReader(const Identity& course, std::span<uint8_t> scratch,
                              HalCompanionFileLookup::Progress progress = nullptr, void* context = nullptr)
      : course(course), scratch(scratch), lookup(progress, context, root.data()) {
    validRoot = courseStateDirectory(course, root);
  }
  ~HalTintaDerivedRecordReader() {
    if (file.isOpen() && !file.close()) LOG_ERR("COMPANION", "Tinta record reader close failed");
  }
  bool matchesCourse(const Identity& expected) const { return validRoot && course == expected; }
  TintaPublicationState inspect(TintaDerivedRecord record, std::span<const uint8_t> expected) {
    TintaDerivedManifestView manifest;
    if (!validRoot || scratch.size() < TINTA_DERIVED_MANIFEST_SIZE || !manifest.decode(expected) ||
        !std::equal(course.begin(), course.end(), expected.begin() + 4))
      return error("arguments");
    const auto loaded = readRecord(record);
    if (loaded == TintaDerivedRecordLoad::Missing) return TintaPublicationState::Missing;
    if (loaded == TintaDerivedRecordLoad::Invalid) return TintaPublicationState::Other;
    if (loaded == TintaDerivedRecordLoad::IoError) return TintaPublicationState::Error;
    return std::equal(expected.begin(), expected.end(), scratch.begin()) ? TintaPublicationState::Matches
                                                                         : TintaPublicationState::Other;
  }
  // Output must not overlap scratch; it is changed only after complete validation.
  TintaDerivedRecordLoad load(TintaDerivedRecord record, std::span<uint8_t> output) {
    if (output.size() < TINTA_DERIVED_MANIFEST_SIZE || scratch.size() < TINTA_DERIVED_MANIFEST_SIZE)
      return loadError("output bounds");
    const auto destination = reinterpret_cast<uintptr_t>(output.data());
    const auto temporary = reinterpret_cast<uintptr_t>(scratch.data());
    const auto distance = destination > temporary ? destination - temporary : temporary - destination;
    if (destination >= temporary ? distance < scratch.size() : distance < TINTA_DERIVED_MANIFEST_SIZE)
      return loadError("output aliases scratch");
    const auto result = readRecord(record);
    if (result == TintaDerivedRecordLoad::Loaded)
      std::copy_n(scratch.begin(), TINTA_DERIVED_MANIFEST_SIZE, output.begin());
    return result;
  }

 private:
  TintaDerivedRecordLoad readRecord(TintaDerivedRecord record) {
    if (!validRoot || scratch.size() < TINTA_DERIVED_MANIFEST_SIZE || !tintaDerivedRecordPath(course, record, path) ||
        (file.isOpen() && !file.close()))
      return loadError("arguments or close");
    const auto presence = lookup.inspect(path.data());
    if (presence == CompanionFilePresence::Missing) return TintaDerivedRecordLoad::Missing;
    if (presence == CompanionFilePresence::Error) return loadError("lookup");
    if (!Storage.openFileForReadReusing("COMPANION", path.data(), file)) return loadError("open");
    const uint64_t length = file.fileSize64();
    if (file.isDirectory() || length != TINTA_DERIVED_MANIFEST_SIZE) {
      if (!file.close()) return loadError("close conflicting record");
      LOG_ERR("COMPANION", "Invalid Tinta publication record type or extent");
      return TintaDerivedRecordLoad::Invalid;
    }
    const bool read =
        file.read(scratch.data(), TINTA_DERIVED_MANIFEST_SIZE) == static_cast<int>(TINTA_DERIVED_MANIFEST_SIZE) &&
        file.fileSize64() == length;
    const bool closed = file.close();
    if (!read || !closed) return loadError("read or close");
    TintaDerivedManifestView stored;
    if (!stored.decode(scratch.first(TINTA_DERIVED_MANIFEST_SIZE)) ||
        !std::equal(course.begin(), course.end(), scratch.begin() + 4)) {
      LOG_ERR("COMPANION", "Invalid Tinta publication record manifest or course");
      return TintaDerivedRecordLoad::Invalid;
    }
    return TintaDerivedRecordLoad::Loaded;
  }

  static TintaDerivedRecordLoad loadError(const char* reason) {
    error(reason);
    return TintaDerivedRecordLoad::IoError;
  }
  static TintaPublicationState error(const char* reason) {
    LOG_ERR("COMPANION", "Tinta publication record %s failed", reason);
    return TintaPublicationState::Error;
  }
  Identity course;
  std::span<uint8_t> scratch;
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  HalCompanionFileLookup lookup;
  HalFile file;
  bool validRoot = false;
};
}  // namespace companion

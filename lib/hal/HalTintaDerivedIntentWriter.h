#pragma once

#include "HalTintaDerivedRecordReader.h"

namespace companion {
// Caller owns the reserved stage path, excludes writers and has validated the full generation.
class HalTintaDerivedIntentWriter {
 public:
  HalTintaDerivedIntentWriter(const Identity& course, HalTintaDerivedRecordReader& reader)
      : course(course), reader(reader) {
    pathsValid = tintaDerivedRecordPath(course, TintaDerivedRecord::Intent, intent) &&
                 tintaDerivedRecordPath(course, TintaDerivedRecord::IntentStage, stage);
  }
  ~HalTintaDerivedIntentWriter() {
    if (file.isOpen() && !file.close()) LOG_ERR("COMPANION", "Tinta intent writer close failed");
  }
  bool persist(std::span<const uint8_t> expected) {
    TintaDerivedManifestView manifest;
    if (!pathsValid || !manifest.decode(expected) || !std::equal(course.begin(), course.end(), expected.begin() + 4) ||
        (file.isOpen() && !file.close()))
      return failure("arguments or close");
    const auto current = reader.inspect(TintaDerivedRecord::Intent, expected);
    if (current == TintaPublicationState::Matches) return true;
    if (current != TintaPublicationState::Missing) return failure("existing intent");
    if (!Storage.openFileForWriteReusing("COMPANION", stage.data(), file) || file.isDirectory())
      return failure("stage open");
    const bool written = file.seek64(0) && file.write(expected.data(), expected.size()) == expected.size() &&
                         file.truncate(expected.size()) && file.sync();
    const bool closed = file.close();
    if (!written || !closed) return failure("stage write/sync/close");
    if (reader.inspect(TintaDerivedRecord::IntentStage, expected) != TintaPublicationState::Matches)
      return failure("stage readback");
    const auto beforeRename = reader.inspect(TintaDerivedRecord::Intent, expected);
    if (beforeRename == TintaPublicationState::Matches) return true;
    if (beforeRename != TintaPublicationState::Missing) return failure("intent ownership");
    if (!Storage.rename(stage.data(), intent.data())) return failure("stage rename");
    if (reader.inspect(TintaDerivedRecord::Intent, expected) != TintaPublicationState::Matches)
      return failure("intent readback");
    return true;
  }

 private:
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Tinta intent publication %s failed", reason);
    return false;
  }
  Identity course;
  HalTintaDerivedRecordReader& reader;
  std::array<char, COURSE_STATE_PATH_SIZE> intent{}, stage{};
  HalFile file;
  bool pathsValid = false;
};
}  // namespace companion

#pragma once

#include "HalTintaDerivedRecordReader.h"

namespace companion {
// Caller has rechecked all active receipts and exclusively owns private record stages.
class HalTintaDerivedReceiptWriter {
 public:
  HalTintaDerivedReceiptWriter(const Identity& course, HalTintaDerivedRecordReader& reader)
      : course(course), reader(reader) {
    pathsValid = tintaDerivedRecordPath(course, TintaDerivedRecord::Receipt, receipt) &&
                 tintaDerivedRecordPath(course, TintaDerivedRecord::ReceiptStage, stage) &&
                 tintaDerivedRecordPath(course, TintaDerivedRecord::Intent, intent);
  }
  ~HalTintaDerivedReceiptWriter() {
    if (file.isOpen() && !file.close()) LOG_ERR("COMPANION", "Tinta receipt writer close failed");
  }
  bool commit(std::span<const uint8_t> expected, std::span<const uint8_t> previous = {}) {
    TintaDerivedManifestView manifest;
    if (!valid(expected, manifest) || (file.isOpen() && !file.close())) return failure("arguments or close");
    const auto current = reader.inspect(TintaDerivedRecord::Receipt, expected);
    if (current == TintaPublicationState::Matches) return true;
    if (current == TintaPublicationState::Error ||
        reader.inspect(TintaDerivedRecord::Intent, expected) != TintaPublicationState::Matches)
      return failure("receipt or intent");
    if (current == TintaPublicationState::Other) {
      TintaDerivedManifestView old;
      if (!valid(previous, old) || old.revision() >= manifest.revision() ||
          !std::equal(previous.begin() + 100, previous.begin() + 116, expected.begin() + 100) ||
          reader.inspect(TintaDerivedRecord::Receipt, previous) != TintaPublicationState::Matches)
        return failure("previous receipt ownership");
    }
    if (!Storage.openFileForWriteReusing("COMPANION", stage.data(), file) || file.isDirectory())
      return failure("stage open");
    const bool written = file.seek64(0) && file.write(expected.data(), expected.size()) == expected.size() &&
                         file.truncate(expected.size()) && file.sync();
    const bool closed = file.close();
    if (!written || !closed ||
        reader.inspect(TintaDerivedRecord::ReceiptStage, expected) != TintaPublicationState::Matches)
      return failure("stage write/readback");
    if (current == TintaPublicationState::Other && !Storage.remove(receipt.data()))
      return failure("old receipt remove");
    if (!Storage.rename(stage.data(), receipt.data())) return failure("receipt rename");
    if (reader.inspect(TintaDerivedRecord::Receipt, expected) != TintaPublicationState::Matches)
      return failure("receipt readback");
    return true;
  }
  // Caller has completed backup cleanup before clearing the recovery gate.
  bool clearIntent(std::span<const uint8_t> expected) {
    TintaDerivedManifestView manifest;
    if (!valid(expected, manifest) ||
        reader.inspect(TintaDerivedRecord::Receipt, expected) != TintaPublicationState::Matches)
      return failure("clear receipt");
    const auto current = reader.inspect(TintaDerivedRecord::Intent, expected);
    if (current == TintaPublicationState::Missing) return true;
    if (current != TintaPublicationState::Matches || !Storage.remove(intent.data())) return failure("clear intent");
    if (reader.inspect(TintaDerivedRecord::Intent, expected) != TintaPublicationState::Missing)
      return failure("clear readback");
    return true;
  }

 private:
  bool valid(std::span<const uint8_t> bytes, TintaDerivedManifestView& view) const {
    return pathsValid && view.decode(bytes) && std::equal(course.begin(), course.end(), bytes.begin() + 4);
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Tinta receipt publication %s failed", reason);
    return false;
  }
  Identity course;
  HalTintaDerivedRecordReader& reader;
  std::array<char, COURSE_STATE_PATH_SIZE> receipt{}, stage{}, intent{};
  HalFile file;
  bool pathsValid = false;
};
}  // namespace companion

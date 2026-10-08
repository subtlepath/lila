#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionTintaReplayPaths.h"
#include "HalTintaCompletionSetView.h"
#include "HalTintaReplayStore.h"

namespace companion {
// Session-owned output; caller freezes replay state and excludes candidate/publication writers.
class HalTintaReplayCompletionExport {
 public:
  explicit HalTintaReplayCompletionExport(TintaReplayExportTarget target = TintaReplayExportTarget::Candidate)
      : target(target) {}
  ~HalTintaReplayCompletionExport() { close(); }
  bool run(HalTintaReplayStore& store, const Identity& course, TintaCompletionKind kind) {
    sealed = false;
    if (!close() || !store.matchesCourse(course) ||
        (kind != TintaCompletionKind::Lessons && kind != TintaCompletionKind::Readings))
      return failure("binding");
    const auto part = kind == TintaCompletionKind::Lessons ? TintaDerivedFile::Lessons : TintaDerivedFile::Readings;
    if (!tintaReplayExportPath(course, part, target, path)) return failure("path");
    const auto eventKind =
        kind == TintaCompletionKind::Lessons ? EventKind::LessonComplete : EventKind::ReadingComplete;
    uint32_t key = 0, count = 0;
    bool previous = false, found = false;
    for (;;) {
      if (!next(store, eventKind, previous, key, found)) return failure("count");
      if (!found) break;
      if (++count > UINT16_MAX) return failure("count limit");
    }
    if (!Storage.openFileForWriteReusing("COMPANION", path.data(), file)) return failure("output open");
    bytes.fill(0);
    bytes[0] = 'T';
    bytes[1] = 'C';
    bytes[2] = 'S';
    bytes[3] = '1';
    bytes[4] = static_cast<uint8_t>(kind);
    tinta_body_detail::write(bytes, 8, count, 4);
    uint32_t crc = binary_record::crc32(bytes.data(), 12);
    if (!write(std::span(bytes).first(12))) return failure("header write");
    key = 0;
    previous = false;
    uint32_t written = 0;
    for (;;) {
      if (!next(store, eventKind, previous, key, found)) return failure("enumeration");
      if (!found) break;
      if (++written > count) return failure("changed count");
      tinta_body_detail::write(bytes, 0, key, 4);
      crc = binary_record::crc32Update(crc, bytes.data(), 4);
      if (!write(std::span(bytes).first(4))) return failure("identity write");
    }
    tinta_body_detail::write(bytes, 0, crc, 4);
    if (written != count || !write(std::span(bytes).first(4)) || !file.sync() || !close())
      return failure("final write");
    if (!Storage.openFileForReadReusing("COMPANION", path.data(), file)) return failure("readback open");
    HalTintaCompletionSetView view(file);
    if (!view.begin(kind, bytes) || !close()) return failure("readback validation");
    sealed = true;
    return true;
  }
  const char* verifiedPath() const { return sealed ? path.data() : nullptr; }

 private:
  static bool next(HalTintaReplayStore& store, EventKind kind, bool& previous, uint32_t& key, bool& found) {
    const auto type =
        kind == EventKind::LessonComplete ? HalTintaReplayStore::Kind::Lesson : HalTintaReplayStore::Kind::Reading;
    for (;;) {
      if (!store.nextKey(type, previous, key, key, found)) return false;
      if (!found) return true;
      previous = true;
      bool enabled = false;
      if (!store.completed(kind, key, enabled)) return false;
      if (enabled) return true;
    }
  }
  bool write(std::span<const uint8_t> value) { return file.write(value.data(), value.size()) == value.size(); }
  bool close() {
    const bool ok = !file || file.close();
    if (!ok) LOG_ERR("COMPANION", "Replay completion export close failed");
    return ok;
  }
  bool failure(const char* stage) {
    sealed = false;
    LOG_ERR("COMPANION", "Replay completion export %s failed", stage);
    close();
    return false;
  }
  const TintaReplayExportTarget target;
  HalFile file;
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  std::array<uint8_t, 80> bytes{};
  bool sealed = false;
};
}  // namespace companion

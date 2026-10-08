#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionTintaReplayPaths.h"
#include "HalTintaItemSnapshotValidation.h"
#include "HalTintaReplayStore.h"

namespace companion {
// Session-owned handles/buffers; caller excludes publication and candidate writers.
class HalTintaReplayItemExport {
 public:
  explicit HalTintaReplayItemExport(TintaReplayExportTarget target = TintaReplayExportTarget::Candidate)
      : target(target) {}
  ~HalTintaReplayItemExport() { close(); }
  bool run(HalTintaReplayStore& store, const Identity& course, uint16_t studyDay) {
    sealed = false;
    if (!close() || !store.matchesCourse(course) ||
        !tintaReplayExportPath(course, TintaDerivedFile::Items, target, path))
      return failure("binding");
    uint32_t count = 0, key = 0;
    bool previous = false, found = false;
    for (;;) {
      if (!store.nextKey(HalTintaReplayStore::Kind::Item, previous, key, key, found)) return failure("count");
      if (!found) break;
      if (++count > 32767) return failure("record limit");
      previous = true;
    }
    TintaReplayDay totals;
    if (!store.day(studyDay, totals) || !Storage.openFileForWriteReusing("COMPANION", path.data(), file))
      return failure("day or output open");
    for (unsigned slot = 0; slot < 2; ++slot) {
      bytes.fill(0);
      bytes[0] = 'T';
      bytes[1] = 'I';
      bytes[2] = 'S';
      bytes[3] = '1';
      tinta_body_detail::write(bytes, 4, 1, 2);
      tinta_body_detail::write(bytes, 6, 80, 2);
      tinta_body_detail::write(bytes, 8, slot + 1, 4);
      tinta_body_detail::write(bytes, 12, count, 4);
      tinta_body_detail::write(bytes, 20, studyDay, 2);
      tinta_body_detail::write(bytes, 22, std::min<uint32_t>(totals.newItems, UINT16_MAX), 2);
      tinta_body_detail::write(bytes, 24, std::min<uint32_t>(totals.reviews, UINT16_MAX), 2);
      tinta_body_detail::write(bytes, 76, binary_record::crc32(bytes.data(), 76), 4);
      if (!write(bytes)) return failure("header write");
      bytes.fill(0);
      for (unsigned remaining = 432; remaining;) {
        const unsigned size = std::min<unsigned>(remaining, bytes.size());
        if (!write(std::span(bytes).first(size))) return failure("padding write");
        remaining -= size;
      }
    }
    key = 0;
    previous = false;
    uint32_t written = 0;
    for (;;) {
      if (!store.nextKey(HalTintaReplayStore::Kind::Item, previous, key, key, found)) return failure("enumeration");
      if (!found) break;
      tinta::core::ItemState item;
      if (++written > count || !store.item(key, item)) return failure("changed items");
      item.encode(bytes.data());
      if (!write(std::span(bytes).first(16))) return failure("item write");
      previous = true;
    }
    if (written != count || !file.sync() || !close()) return failure("sync or changed count");
    if (!Storage.openFileForReadReusing("COMPANION", path.data(), file) ||
        !validateTintaItemSnapshot(file, studyDay, bytes) || !close())
      return failure("readback");
    sealed = true;
    return true;
  }
  const char* verifiedPath() const { return sealed ? path.data() : nullptr; }

 private:
  bool write(std::span<const uint8_t> value) { return file.write(value.data(), value.size()) == value.size(); }
  bool close() {
    const bool ok = !file || file.close();
    if (!ok) LOG_ERR("COMPANION", "Replay item export close failed");
    return ok;
  }
  bool failure(const char* stage) {
    sealed = false;
    LOG_ERR("COMPANION", "Replay item export %s failed", stage);
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

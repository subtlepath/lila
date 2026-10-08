#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionTintaReplayPaths.h"
#include "HalTintaDayLogValidation.h"
#include "HalTintaReplayStore.h"

namespace companion {
// Session-owned output; caller freezes replay state and excludes publication/candidate writers.
class HalTintaReplayDayExport {
 public:
  explicit HalTintaReplayDayExport(TintaReplayExportTarget target = TintaReplayExportTarget::Candidate)
      : target(target) {}
  ~HalTintaReplayDayExport() { close(); }
  bool run(HalTintaReplayStore& store, const Identity& course) {
    sealed = false;
    if (!close() || !store.matchesCourse(course) ||
        !tintaReplayExportPath(course, TintaDerivedFile::Days, target, path) ||
        !Storage.openFileForWriteReusing("COMPANION", path.data(), file))
      return failure("binding or open");
    static constexpr uint8_t HEADER[] = {'T', 'D', 'L', '1'};
    if (file.write(HEADER, sizeof HEADER) != sizeof HEADER) return failure("header write");
    uint64_t extent = 4;
    uint32_t key = 0;
    bool previous = false, found = false;
    unsigned records = 0;
    for (;;) {
      if (!store.nextKey(HalTintaReplayStore::Kind::Day, previous, key, key, found)) return failure("enumeration");
      if (!found) break;
      TintaReplayDay totals;
      if (key > UINT16_MAX || !store.day(static_cast<uint16_t>(key), totals)) return failure("day read");
      const uint64_t seconds = totals.responseMilliseconds / 1000 + (totals.responseMilliseconds % 1000 >= 500);
      if (seconds > UINT32_MAX) return failure("seconds overflow");
      std::array<uint32_t, 4> remaining{totals.gradedReviews, totals.correctReviews, totals.newItems,
                                        static_cast<uint32_t>(seconds)};
      do {
        if (extent > UINT32_MAX - 12) return failure("extent overflow");
        tinta_body_detail::write(bytes, 0, key, 2);
        for (unsigned at = 0; at < remaining.size(); ++at) {
          const auto part = std::min<uint32_t>(remaining[at], UINT16_MAX);
          tinta_body_detail::write(bytes, 2 + at * 2, part, 2);
          remaining[at] -= part;
        }
        tinta_body_detail::write(bytes, 10, binary_record::crc32(bytes.data(), 10) & UINT16_MAX, 2);
        if (file.write(bytes.data(), bytes.size()) != bytes.size()) return failure("record write");
        extent += 12;
        if (++records == 32) {
          records = 0;
          vTaskDelay(1);
        }
      } while (std::any_of(remaining.begin(), remaining.end(), [](uint32_t value) { return value != 0; }));
      previous = true;
    }
    if (file.fileSize64() != extent || !file.sync() || !close()) return failure("sync or extent");
    if (!Storage.openFileForReadReusing("COMPANION", path.data(), file) ||
        !validateTintaDayLogFile(file, bytes, yieldProgress) || !close())
      return failure("readback");
    sealed = true;
    return true;
  }
  const char* verifiedPath() const { return sealed ? path.data() : nullptr; }

 private:
  static bool yieldProgress(void*) {
    vTaskDelay(1);
    return true;
  }
  bool close() {
    const bool ok = !file || file.close();
    if (!ok) LOG_ERR("COMPANION", "Replay day export close failed");
    return ok;
  }
  bool failure(const char* stage) {
    sealed = false;
    LOG_ERR("COMPANION", "Replay day export %s failed", stage);
    close();
    return false;
  }
  const TintaReplayExportTarget target;
  HalFile file;
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  std::array<uint8_t, 12> bytes{};
  bool sealed = false;
};
}  // namespace companion

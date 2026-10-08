#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "HalInventoryFileHash.h"
#include "HalTintaReplayCompletionExport.h"
#include "HalTintaReplayDayExport.h"
#include "HalTintaReplayItemExport.h"

namespace companion {
// Session-owned workspace; caller freezes replay and excludes publication/candidate writers.
class HalTintaReplayExport {
 public:
  explicit HalTintaReplayExport(TintaReplayExportTarget target = TintaReplayExportTarget::Candidate)
      : target(target), items(target), completions(target), days(target) {}
  ~HalTintaReplayExport() { close(); }
  bool run(HalTintaReplayStore& store, const Identity& course, uint16_t studyDay, std::span<uint8_t> scratch) {
    sealed = false;
    if (!close() || scratch.size() < 80 || !store.matchesCourse(course)) return failure("arguments");
    if (!items.run(store, course, studyDay) || !completions.run(store, course, TintaCompletionKind::Lessons) ||
        !completions.run(store, course, TintaCompletionKind::Readings) || !days.run(store, course))
      return failure("snapshot export");
    if (!tintaReplayExportPath(course, TintaDerivedFile::LocalReviews, target, path) ||
        !Storage.openFileForWriteReusing("COMPANION", path.data(), file) || file.fileSize64() != 0 || !file.sync() ||
        !close())
      return failure("empty review log");
    for (unsigned at = 0; at < receipts.size(); ++at) {
      const auto kind = static_cast<TintaDerivedFile>(at);
      if (!tintaReplayExportPath(course, kind, target, path) ||
          !Storage.openFileForReadReusing("COMPANION", path.data(), file) ||
          !hashInventoryFile(file, scratch, receipts[at].length, receipts[at].hash) || !close())
        return failure("snapshot hash");
    }
    exportedCourse = course;
    exportedDay = studyDay;
    sealed = true;
    return true;
  }
  bool receipt(TintaDerivedFile kind, uint64_t& length, Digest& hash) const {
    const auto at = static_cast<unsigned>(kind);
    if (!sealed || at >= receipts.size()) {
      LOG_ERR("COMPANION", "Replay export receipt unavailable");
      return false;
    }
    length = receipts[at].length;
    hash = receipts[at].hash;
    return true;
  }
  bool manifest(const Identity& storage, const Digest& pack, const Digest& frontier, const Identity& snapshot,
                uint64_t revision, std::span<uint8_t> output) {
    if (!sealed || output.size() < TINTA_DERIVED_MANIFEST_SIZE || !tinta_body_detail::nonzero(storage) ||
        !tinta_body_detail::nonzero(pack) || !tinta_body_detail::nonzero(frontier) ||
        !tinta_body_detail::nonzero(snapshot) || !revision) {
      LOG_ERR("COMPANION", "Replay export manifest arguments invalid");
      return false;
    }
    manifestBytes.fill(0);
    manifestBytes[0] = 'T';
    manifestBytes[1] = 'D';
    manifestBytes[2] = 'S';
    manifestBytes[3] = '1';
    std::copy(exportedCourse.begin(), exportedCourse.end(), manifestBytes.begin() + 4);
    std::copy(pack.begin(), pack.end(), manifestBytes.begin() + 20);
    std::copy(frontier.begin(), frontier.end(), manifestBytes.begin() + 52);
    std::copy(snapshot.begin(), snapshot.end(), manifestBytes.begin() + 84);
    std::copy(storage.begin(), storage.end(), manifestBytes.begin() + 100);
    tinta_body_detail::write(manifestBytes, 116, exportedDay, 2);
    tinta_body_detail::write(manifestBytes, 120, revision, 8);
    for (unsigned at = 0; at < receipts.size(); ++at) {
      tinta_body_detail::write(manifestBytes, 128 + at * 40, receipts[at].length, 8);
      std::copy(receipts[at].hash.begin(), receipts[at].hash.end(), manifestBytes.begin() + 136 + at * 40);
    }
    tinta_body_detail::write(manifestBytes, 328, binary_record::crc32(manifestBytes.data(), 328), 4);
    TintaDerivedManifestView checked;
    if (!checked.decode(manifestBytes) || !checked.matches(exportedCourse, storage, pack, frontier)) {
      LOG_ERR("COMPANION", "Replay export manifest validation failed");
      return false;
    }
    std::copy(manifestBytes.begin(), manifestBytes.end(), output.begin());
    return true;
  }
  bool complete() const { return sealed; }

 private:
  bool close() {
    const bool ok = !file || file.close();
    if (!ok) LOG_ERR("COMPANION", "Replay export close failed");
    return ok;
  }
  bool failure(const char* stage) {
    sealed = false;
    LOG_ERR("COMPANION", "Replay export %s failed", stage);
    close();
    return false;
  }
  struct Receipt {
    uint64_t length = 0;
    Digest hash{};
  };
  std::array<Receipt, 5> receipts{};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> manifestBytes{};
  Identity exportedCourse{};
  uint16_t exportedDay = 0;
  const TintaReplayExportTarget target;
  HalTintaReplayItemExport items;
  HalTintaReplayCompletionExport completions;
  HalTintaReplayDayExport days;
  HalFile file;
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  bool sealed = false;
};
}  // namespace companion

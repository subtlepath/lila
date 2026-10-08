#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CompanionJournalReplayOrder.h"

namespace companion {
// Session-owned, disposable bitset. Every replay starts with a fresh reset.
class HalJournalReplayVisits final : public JournalReplayVisits {
 public:
  explicit HalJournalReplayVisits(bool undoExclusions = false) : path(undoExclusions ? UNDO_PATH : PATH) {}
  ~HalJournalReplayVisits() { close(); }
  bool reset(uint32_t records) override {
    if (!close()) return false;
    if (records > UINT32_MAX / TintaJournal::RECORD_SIZE || !Storage.ready() ||
        !Storage.ensureDirectoryExists("/.crosspoint/companion") ||
        !Storage.openFileForWriteReusing("COMPANION", path, file))
      return failure("replay visits reset");
    const uint32_t bytes = (records + 7) / 8;
    for (uint32_t offset = 0; offset < bytes;) {
      const auto size = std::min<uint32_t>(zeros.size(), bytes - offset);
      if (file.write(zeros.data(), size) != size) return failure("replay visits initialize");
      offset += size;
      yield();
    }
    if (!file.sync() || file.fileSize64() != bytes) return failure("replay visits sync or extent");
    count = records;
    ready = true;
    return true;
  }
  bool visited(uint32_t record, bool& value) override {
    uint8_t byte = 0;
    if (!readByte(record, byte)) return false;
    value = (byte & (1U << (record % 8))) != 0;
    return true;
  }
  bool mark(uint32_t record) override {
    uint8_t byte = 0;
    if (!readByte(record, byte)) return false;
    byte |= 1U << (record % 8);
    if (!file.seek64(record / 8) || file.write(&byte, 1) != 1 || !file.sync()) return failure("replay visits mark");
    yield();
    return true;
  }
  bool close() {
    ready = false;
    count = 0;
    operations = 0;
    const bool ok = !file || file.close();
    if (!ok) LOG_ERR("COMPANION", "Replay visits close failed");
    return ok;
  }
  static constexpr char UNDO_PATH[] = "/.crosspoint/companion/journal-replay-undone";
  static constexpr char PATH[] = "/.crosspoint/companion/journal-replay-visits";

 private:
  bool readByte(uint32_t record, uint8_t& byte) {
    if (!ready || record >= count || !file.seek64(record / 8) || file.read(&byte, 1) != 1)
      return failure("replay visits read");
    yield();
    return true;
  }
  bool failure(const char* message) {
    ready = false;
    LOG_ERR("COMPANION", "%s", message);
    return false;
  }
  void yield() {
    if (++operations == 32) {
      operations = 0;
      vTaskDelay(1);
    }
  }
  const char* path;
  HalFile file;
  static constexpr std::array<uint8_t, 64> zeros{};
  uint32_t count = 0;
  unsigned operations = 0;
  bool ready = false;
};
}  // namespace companion

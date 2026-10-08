#include "HalInventorySortStorage.h"

#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <climits>
#include <cstring>
#include <limits>

#include "CompanionTransfer.h"

namespace companion {
HalInventorySortStorage::~HalInventorySortStorage() { close(); }

void HalInventorySortStorage::yieldPeriodically() {
  if (++operationsSinceYield == 32) {
    operationsSinceYield = 0;
    vTaskDelay(1);
  }
}

bool HalInventorySortStorage::failure(const char* operation) {
  failed = true;
  LOG_ERR("COMPANION", "Inventory sort %s failed", operation);
  return false;
}
bool HalInventorySortStorage::reset() {
  failed = true;
  operationsSinceYield = 0;
  lengths.fill(0);
  finished.fill(false);
  if (!runPaths[0] || !runPaths[1] || std::strcmp(runPaths[0], runPaths[1]) == 0) return failure("run paths");
  if (!Storage.ready() || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY)) return failure("prepare");
  for (unsigned run = 0; run < files.size(); ++run) {
    if (!files[run]) files[run] = Storage.open(runPaths[run], O_RDWR | O_CREAT);
    if (!files[run] || files[run].isDirectory() || !files[run].truncate(0) || !files[run].sync())
      return failure("reset");
  }
  failed = false;
  return true;
}
bool HalInventorySortStorage::read(unsigned run, uint64_t offset, std::span<uint8_t> bytes) {
  if (failed || run >= files.size() || !finished[run] || !files[run] || bytes.size() > INT_MAX ||
      offset > lengths[run] || bytes.size() > lengths[run] - offset)
    return failure("read bounds");
  if (bytes.empty()) return true;
  if (!files[run].seek64(offset) || files[run].read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size()))
    return failure("read");
  yieldPeriodically();
  return true;
}
bool HalInventorySortStorage::write(unsigned run, uint64_t offset, std::span<const uint8_t> bytes) {
  if (failed || run >= files.size() || !files[run] || bytes.size() > std::numeric_limits<uint64_t>::max() - offset)
    return failure("write bounds");
  if (finished[run]) {
    if (offset != 0) return failure("restart offset");
    lengths[run] = 0;
    finished[run] = false;
  }
  if (offset != lengths[run]) return failure("write offset");
  if (!files[run].seek64(offset) || files[run].write(bytes.data(), bytes.size()) != bytes.size())
    return failure("write");
  lengths[run] += bytes.size();
  if (!bytes.empty()) yieldPeriodically();
  return true;
}
bool HalInventorySortStorage::finish(unsigned run, uint64_t size) {
  if (failed || run >= files.size() || !files[run] || size != lengths[run]) return failure("finish bounds");
  if (!files[run].truncate(size) || !files[run].sync() || files[run].fileSize64() != size) return failure("finish");
  finished[run] = true;
  return true;
}
bool HalInventorySortStorage::close() {
  bool ok = true;
  failed = true;
  finished.fill(false);
  lengths.fill(0);
  for (auto& file : files) {
    if (file && !file.close()) {
      LOG_ERR("COMPANION", "Inventory sort close failed");
      ok = false;
    }
    file = HalFile();
  }
  return ok;
}
}  // namespace companion

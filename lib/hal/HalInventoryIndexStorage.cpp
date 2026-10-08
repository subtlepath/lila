#include "HalInventoryIndexStorage.h"

#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <climits>

namespace companion {
HalInventoryIndexStorage::~HalInventoryIndexStorage() { close(); }

bool HalInventoryIndexStorage::open(const char* path) {
  if (!close()) return false;
  if (!path || !path[0] || !Storage.openFileForRead("COMPANION", path, file) || file.isDirectory()) {
    LOG_ERR("COMPANION", "Cannot open inventory snapshot");
    close();
    return false;
  }
  length = file.fileSize64();
  return true;
}

bool HalInventoryIndexStorage::close() {
  const bool ok = !file || file.close();
  if (!ok) LOG_ERR("COMPANION", "Inventory snapshot close failed");
  file = HalFile();
  length = 0;
  readsSinceYield = 0;
  failed = false;
  return ok;
}

bool HalInventoryIndexStorage::size(uint64_t& bytes) {
  if (!file || failed) {
    LOG_ERR("COMPANION", "Inventory snapshot unavailable");
    return false;
  }
  bytes = length;
  return true;
}

bool HalInventoryIndexStorage::read(uint64_t offset, std::span<uint8_t> output) {
  if (!file || failed || output.size() > INT_MAX || offset > length || output.size() > length - offset) {
    LOG_ERR("COMPANION", "Invalid inventory snapshot read");
    return false;
  }
  if (output.empty()) return true;
  if (!file.seek64(offset) || file.read(output.data(), output.size()) != static_cast<int>(output.size())) {
    failed = true;
    LOG_ERR("COMPANION", "Inventory snapshot read failed");
    return false;
  }
  if (++readsSinceYield == 32) {
    readsSinceYield = 0;
    vTaskDelay(1);
  }
  return true;
}
}  // namespace companion

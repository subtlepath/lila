#include "HalInventoryFileHash.h"

#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <climits>

namespace companion {
bool hashInventoryFile(HalFile& file, std::span<uint8_t> scratch, uint64_t& length, Digest& hash,
                       InventoryHashProgress progress, void* progressContext) {
  if (!file || file.isDirectory() || scratch.empty() || scratch.size() > INT_MAX || !file.seek64(0)) {
    LOG_ERR("COMPANION", "Invalid inventory hash input");
    return false;
  }
  const uint64_t size = file.fileSize64();
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  int result = mbedtls_sha256_starts(&context, 0);
  uint64_t remaining = size;
  while (result == 0 && remaining != 0) {
    const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, scratch.size()));
    if ((progress && !progress(progressContext)) || file.read(scratch.data(), count) != static_cast<int>(count)) {
      result = -1;
      break;
    }
    result = mbedtls_sha256_update(&context, scratch.data(), count);
    remaining -= count;
    vTaskDelay(1);
  }
  Digest actual{};
  if (result == 0) result = mbedtls_sha256_finish(&context, actual.data());
  mbedtls_sha256_free(&context);
  if (result != 0 || (progress && !progress(progressContext)) || !file || file.fileSize64() != size) {
    LOG_ERR("COMPANION", "Inventory SHA-256 read or size verification failed");
    return false;
  }
  length = size;
  hash = actual;
  return true;
}
}  // namespace companion

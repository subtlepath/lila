#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <climits>

#include "CompanionInventoryIndex.h"

namespace companion {
// Borrows the walker's current file without allocating or closing another handle.
class HalInventoryFileView final : public InventoryIndexStorage {
 public:
  bool attach(HalFile& borrowed) {
    file = &borrowed;
    failed = true;
    reads = 0;
    if (!borrowed || borrowed.isDirectory()) return failure("attach");
    length = borrowed.fileSize64();
    failed = false;
    return true;
  }
  bool size(uint64_t& output) override {
    if (failed || !file || !*file) return failure("size");
    output = length;
    return true;
  }
  bool read(uint64_t offset, std::span<uint8_t> output) override {
    if (failed || !file || !*file || output.size() > INT_MAX || offset > length || output.size() > length - offset)
      return failure("bounds");
    if (output.empty()) return true;
    if (!file->seek64(offset) || file->read(output.data(), output.size()) != static_cast<int>(output.size()))
      return failure("read");
    if (++reads == 32) {
      reads = 0;
      vTaskDelay(1);
    }
    return true;
  }

 private:
  HalFile* file = nullptr;
  uint64_t length = 0;
  uint8_t reads = 0;
  bool failed = true;
  bool failure(const char* operation) {
    failed = true;
    LOG_ERR("COMPANION", "Inventory file view %s failed", operation);
    return false;
  }
};
}  // namespace companion

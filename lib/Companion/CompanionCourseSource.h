#pragma once

#include "CompanionInventoryIndex.h"
#include "core/pack/PackSource.h"

namespace companion {
// Borrows one immutable HAL-backed handle; no cache or whole-pack allocation.
class StoredCourseSource final : public tinta::core::pack::PackSource {
 public:
  explicit StoredCourseSource(InventoryIndexStorage& storage) : storage(storage) {}
  bool attach() {
    length = 0;
    uint64_t bytes = 0;
    if (!storage.size(bytes) || bytes == 0 || bytes > UINT32_MAX) return false;
    length = static_cast<uint32_t>(bytes);
    return true;
  }
  void detach() { length = 0; }
  uint32_t size() const override { return length; }
  bool read(uint32_t offset, void* output, uint32_t count) override {
    if (length == 0 || offset > length || count > length - offset || (count != 0 && output == nullptr)) return false;
    if (!storage.read(offset, {static_cast<uint8_t*>(output), count})) {
      detach();
      return false;
    }
    return true;
  }

 private:
  InventoryIndexStorage& storage;
  uint32_t length = 0;
};
}  // namespace companion

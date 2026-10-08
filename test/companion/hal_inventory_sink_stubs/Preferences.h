#pragma once
#include <HalStorage.h>
class Preferences {
 public:
  bool begin(const char*, bool) { return !inventory_hal_test::state.failNvsOpen; }
  void end() {}
  bool isKey(const char*) { return !inventory_hal_test::state.revisionBytes.empty(); }
  size_t getBytesLength(const char*) { return inventory_hal_test::state.revisionBytes.size(); }
  size_t getBytes(const char*, void* out, size_t size) {
    auto& state = inventory_hal_test::state;
    if (state.failNvsRead) return 0;
    auto count = std::min(size, state.revisionBytes.size());
    std::copy_n(state.revisionBytes.begin(), count, static_cast<uint8_t*>(out));
    return count;
  }
  size_t putBytes(const char*, const void* in, size_t size) {
    if (inventory_hal_test::state.failNvsWrite) return 0;
    const auto* bytes = static_cast<const uint8_t*>(in);
    inventory_hal_test::state.revisionBytes.assign(bytes, bytes + size);
    return size;
  }
};

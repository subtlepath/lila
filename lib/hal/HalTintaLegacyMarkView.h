#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>
#include <span>

#include "../Serialization/BinaryRecordBytes.h"
#include "core/library/MarkLog.h"

namespace companion {
// Borrowed immutable hash-verified file/workspace; caller excludes writers.
// Reconstructs legacy add/remove order without mutating or compacting the log.
class HalTintaLegacyMarkView final {
 public:
  explicit HalTintaLegacyMarkView(HalFile& file) : file(file) {}
  static constexpr size_t WORKSPACE_SIZE = tinta::core::library::MarkLog::kCapacity * 4;
  bool begin(std::span<uint8_t> workspace, bool (*permitted)(void*) = nullptr, void* context = nullptr) {
    ready = false;
    count = 0;
    scratch = workspace;
    permission = permitted;
    permissionContext = context;
    if (!allowed() || !file.isOpen() || file.isDirectory() || scratch.size() < WORKSPACE_SIZE || overlapsOwner())
      return failure("arguments");
    extent = file.fileSize64();
    uint8_t bytes[8];
    if (extent < 4 || extent > UINT32_MAX || (extent - 4) % 8 || !file.seek64(0) || file.read(bytes, 4) != 4 ||
        std::memcmp(bytes, "TMK1", 4))
      return failure("header or extent");
    for (uint64_t offset = 4; offset < extent; offset += 8) {
      if (((offset - 4) / 8) % 32 == 0) vTaskDelay(1);
      if (!allowed() || file.read(bytes, sizeof bytes) != sizeof bytes) return failure("read or cancellation");
      const uint32_t key = binary_record::getU32(bytes);
      if (!key || bytes[5] || (bytes[4] != 1 && bytes[4] != 2) ||
          binary_record::getU16(bytes + 6) != uint16_t(binary_record::crc32(bytes, 6)))
        return failure("record");
      uint16_t position = 0;
      while (position < count && binary_record::getU32(scratch.data() + position * 4) != key) ++position;
      if (bytes[4] == 1 && position == count) {
        if (count == tinta::core::library::MarkLog::kCapacity) return failure("capacity");
        binary_record::putU32(scratch.data() + count++ * 4, key);
      } else if (bytes[4] == 2 && position < count) {
        std::memmove(scratch.data() + position * 4, scratch.data() + (position + 1) * 4, (count - position - 1) * 4);
        --count;
      }
    }
    if (!allowed() || file.fileSize64() != extent) return failure("changed extent or cancellation");
    ready = true;
    return true;
  }
  bool entryCount(uint16_t& output) {
    if (!ready || !allowed() || file.fileSize64() != extent) return failure("count unavailable");
    output = count;
    return true;
  }
  bool identityAt(uint16_t index, uint32_t& output) {
    if (!ready || !allowed() || index >= count || file.fileSize64() != extent) return failure("identity unavailable");
    output = binary_record::getU32(scratch.data() + index * 4);
    return true;
  }

 private:
  HalFile& file;
  std::span<uint8_t> scratch;
  uint64_t extent = 0;
  bool (*permission)(void*) = nullptr;
  void* permissionContext = nullptr;
  uint16_t count = 0;
  bool ready = false;
  bool overlapsOwner() const {
    const auto start = reinterpret_cast<uintptr_t>(scratch.data());
    const auto owner = reinterpret_cast<uintptr_t>(this);
    return start <= owner ? owner - start < scratch.size() : start - owner < sizeof(*this);
  }
  bool allowed() const { return !permission || permission(permissionContext); }
  bool failure([[maybe_unused]] const char* reason) {
    ready = false;
    LOG_ERR("COMPANION", "Legacy mark inspection failed: %s", reason);
    return false;
  }
};
}  // namespace companion

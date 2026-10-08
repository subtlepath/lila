#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>

namespace companion {
class IdentityKeys {
 public:
  virtual ~IdentityKeys() = default;
  virtual uint32_t count() const = 0;
  // The source must be immutable, report I/O errors and yield during long scans.
  virtual bool read(uint32_t index, uint32_t& identity) = 0;
};
// Adaptive radix partitions bound memory without recursion or aligned loads.
inline bool uniqueIdentityKeys(IdentityKeys& source, std::span<uint8_t> scratch) {
  const uint32_t capacity = std::min<size_t>(128, scratch.size() / sizeof(uint32_t));
  if (capacity == 0) return false;
  uint32_t prefix = 0;
  unsigned depth = 0;
  for (;;) {
    uint32_t used = 0;
    bool overflow = false;
    for (uint32_t index = 0; index < source.count(); ++index) {
      uint32_t key = 0;
      if (!source.read(index, key) || key == 0 || key == UINT32_MAX) return false;
      if (depth != 0 && (key >> (32 - depth)) != prefix) continue;
      uint32_t low = 0, high = used;
      while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        uint32_t value = 0;
        std::memcpy(&value, scratch.data() + middle * sizeof(value), sizeof(value));
        if (value < key)
          low = middle + 1;
        else
          high = middle;
      }
      if (low != used) {
        uint32_t value = 0;
        std::memcpy(&value, scratch.data() + low * sizeof(value), sizeof(value));
        if (value == key) return false;
      }
      if (used == capacity) {
        overflow = true;
        break;
      }
      std::memmove(scratch.data() + (low + 1) * sizeof(key), scratch.data() + low * sizeof(key),
                   (used - low) * sizeof(key));
      std::memcpy(scratch.data() + low * sizeof(key), &key, sizeof(key));
      ++used;
    }
    if (overflow) {
      if (depth == 32) return false;
      depth += 4;
      prefix <<= 4;
      continue;
    }
    if (depth == 0) return true;
    while ((prefix & 15) == 15) {
      prefix >>= 4;
      depth -= 4;
      if (depth == 0) return true;
    }
    ++prefix;
  }
}
}  // namespace companion

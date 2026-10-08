#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace companion {
// One loaded book's last checked publication; revision invalidates other writers.
class ReaderProgressSaveCache final {
 public:
  bool matches(uint64_t revision, std::span<const uint8_t> bytes) const {
    return valid && revision == savedRevision && bytes.size() == saved.size() &&
           std::equal(bytes.begin(), bytes.end(), saved.begin());
  }
  void clear() { valid = false; }
  bool remember(uint64_t revision, std::span<const uint8_t> bytes) {
    clear();
    if (bytes.size() != saved.size()) return false;
    std::copy(bytes.begin(), bytes.end(), saved.begin());
    savedRevision = revision;
    valid = true;
    return true;
  }

 private:
  std::array<uint8_t, 10> saved{};
  uint64_t savedRevision = 0;
  bool valid = false;
};
}  // namespace companion

#pragma once

#include "CompanionZipRangeValidation.h"

namespace companion {
class ZipNameBytesStorage {
 public:
  virtual ~ZipNameBytesStorage() = default;
  virtual bool reset() = 0;
  virtual bool read(uint64_t offset, std::span<uint8_t> bytes) = 0;
  virtual bool append(uint64_t offset, std::span<const uint8_t> bytes) = 0;
  virtual bool seal(uint64_t bytes) = 0;
};
// Inputs are already normalized/trimmed UTF-8 names. Stores names and sorts
// offset/length references without retaining the archive's name set in RAM.
class ZipNameDuplicateValidation final {
 public:
  using Progress = bool (*)(void*);
  ZipNameDuplicateValidation(ZipRangeStorage& index, ZipNameBytesStorage& names, std::span<uint8_t> scratch,
                             Progress progress = nullptr, void* context = nullptr)
      : index(index), names(names), scratch(scratch), progress(progress), context(context) {}
  bool begin(uint64_t maximum = 20000) {
    ready = false;
    count = extent = 0;
    if (scratch.size() < 2 || !maximum || maximum > UINT64_MAX / 16 || !tick() || !index.reset() || !names.reset())
      return false;
    this->maximum = maximum;
    ready = true;
    return true;
  }
  bool add(std::span<const uint8_t> name) {
    if (!ready || name.empty() || count == maximum || name.size() > UINT64_MAX - extent) return fail();
    inventory_detail::write(first, 0, extent, 8);
    inventory_detail::write(first, 8, name.size(), 8);
    if (!tick() || !names.append(extent, name) || !tick() || !index.write(count * 16, first)) return fail();
    extent += name.size();
    ++count;
    return true;
  }
  void abort() { ready = false; }
  bool finish() {
    if (!ready) return false;
    ready = false;
    for (uint64_t root = count / 2; root-- > 0;)
      if (!sift(root, count)) return false;
    for (uint64_t end = count; end > 1;) {
      --end;
      if (!swap(0, end) || !sift(0, end)) return false;
    }
    if (count) {
      int order = 0;
      if (!compare(0, 0, order)) return false;
    }
    for (uint64_t at = 1; at < count; ++at) {
      int order = 0;
      if (!compare(at - 1, at, order) || order >= 0) return false;
    }
    return tick() && names.seal(extent) && tick() && index.seal(count * 16);
  }

 private:
  ZipRangeStorage& index;
  ZipNameBytesStorage& names;
  std::span<uint8_t> scratch;
  Progress progress;
  void* context;
  std::array<uint8_t, 16> first{}, second{};
  uint64_t count = 0, extent = 0, maximum = 0;
  bool ready = false;
  bool tick() const { return !progress || progress(context); }
  bool fail() {
    ready = false;
    return false;
  }
  bool read(uint64_t at, std::span<uint8_t> bytes) { return tick() && index.read(at * 16, bytes); }
  bool swap(uint64_t a, uint64_t b) {
    return read(a, first) && read(b, second) && tick() && index.write(a * 16, second) && tick() &&
           index.write(b * 16, first);
  }
  bool compare(uint64_t a, uint64_t b, int& order) {
    if (!read(a, first) || !read(b, second)) return false;
    const auto left = inventory_detail::read(first, 0, 8), right = inventory_detail::read(second, 0, 8);
    const auto leftBytes = inventory_detail::read(first, 8, 8), rightBytes = inventory_detail::read(second, 8, 8);
    if (!leftBytes || !rightBytes || left > extent || right > extent || leftBytes > extent - left ||
        rightBytes > extent - right)
      return false;
    const auto half = scratch.size() / 2;
    for (uint64_t at = 0; at < std::min(leftBytes, rightBytes);) {
      const auto bytes = static_cast<size_t>(std::min<uint64_t>(half, std::min(leftBytes, rightBytes) - at));
      if (!tick() || !names.read(left + at, scratch.first(bytes)) || !tick() ||
          !names.read(right + at, scratch.subspan(half, bytes)))
        return false;
      for (size_t i = 0; i < bytes; ++i)
        if (scratch[i] != scratch[half + i]) {
          order = scratch[i] < scratch[half + i] ? -1 : 1;
          return true;
        }
      at += bytes;
    }
    order = leftBytes < rightBytes ? -1 : leftBytes > rightBytes ? 1 : 0;
    return true;
  }
  bool sift(uint64_t root, uint64_t size) {
    while (root < size / 2) {
      auto child = root * 2 + 1;
      int order = 0;
      if (child + 1 < size) {
        if (!compare(child + 1, child, order)) return false;
        if (order > 0) ++child;
      }
      if (!compare(child, root, order)) return false;
      if (order <= 0) return true;
      if (!swap(root, child)) return false;
      root = child;
    }
    return true;
  }
};
}  // namespace companion

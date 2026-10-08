#pragma once

#include "CompanionInventoryIndex.h"

namespace companion {
// Private session-owned staging storage. Writes replace exactly the supplied
// bytes; reset must not delete unrelated files. No publication happens here.
class ZipRangeStorage {
 public:
  virtual ~ZipRangeStorage() = default;
  virtual bool reset() = 0;
  virtual bool read(uint64_t offset, std::span<uint8_t> bytes) = 0;
  virtual bool write(uint64_t offset, std::span<const uint8_t> bytes) = 0;
  virtual bool seal(uint64_t bytes) = 0;
};
// In-place heapsort of SD-backed intervals. Two fixed records replace an
// archive-sized RAM table. The owner must keep archive and stage immutable to
// other tasks while validating; false requires cleanup/restart by the owner.
class ZipRangeValidation final {
 public:
  using Progress = bool (*)(void*);
  explicit ZipRangeValidation(ZipRangeStorage& storage, Progress progress = nullptr, void* context = nullptr)
      : storage(storage), progress(progress), context(context) {}
  bool begin(uint64_t limit, uint64_t maximum = 20000) {
    ready = false;
    count = 0;
    if (!maximum || maximum > UINT64_MAX / 16 || !tick() || !storage.reset()) return false;
    this->limit = limit;
    this->maximum = maximum;
    ready = true;
    return true;
  }
  bool add(uint64_t start, uint64_t end) {
    if (!ready || count == maximum || start >= end || end > limit) return fail();
    inventory_detail::write(first, 0, start, 8);
    inventory_detail::write(first, 8, end, 8);
    if (!tick() || !storage.write(count * 16, first)) return fail();
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
    uint64_t priorEnd = 0;
    for (uint64_t at = 0; at < count; ++at) {
      if (!read(at, first)) return false;
      const auto start = inventory_detail::read(first, 0, 8), end = inventory_detail::read(first, 8, 8);
      if (start < priorEnd || start >= end || end > limit) return false;
      priorEnd = end;
    }
    return tick() && storage.seal(count * 16);
  }

 private:
  ZipRangeStorage& storage;
  Progress progress;
  void* context;
  uint64_t count = 0, maximum = 0, limit = 0;
  bool ready = false;
  std::array<uint8_t, 16> first{}, second{};
  bool tick() const { return !progress || progress(context); }
  bool fail() {
    ready = false;
    return false;
  }
  bool read(uint64_t index, std::span<uint8_t> out) { return tick() && storage.read(index * 16, out); }
  bool swap(uint64_t a, uint64_t b) {
    return read(a, first) && read(b, second) && tick() && storage.write(a * 16, second) && tick() &&
           storage.write(b * 16, first);
  }
  bool greater(uint64_t a, uint64_t b, bool& result) {
    if (!read(a, first) || !read(b, second)) return false;
    const auto left = inventory_detail::read(first, 0, 8), right = inventory_detail::read(second, 0, 8);
    result =
        left > right || (left == right && inventory_detail::read(first, 8, 8) > inventory_detail::read(second, 8, 8));
    return true;
  }
  bool sift(uint64_t root, uint64_t size) {
    while (root < size / 2) {
      auto child = root * 2 + 1;
      bool larger = false;
      if (child + 1 < size) {
        if (!greater(child + 1, child, larger)) return false;
        if (larger) ++child;
      }
      if (!greater(child, root, larger)) return false;
      if (!larger) return true;
      if (!swap(root, child)) return false;
      root = child;
    }
    return true;
  }
};
}  // namespace companion

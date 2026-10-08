#pragma once

#include <algorithm>
#include <cstring>

#include "CompanionInventoryIndexBuilder.h"

namespace companion {
class UnsortedInventorySource {
 public:
  virtual ~UnsortedInventorySource() = default;
  virtual InventorySourceResult next(ContentManifest& manifest) = 0;
};
// Two disposable runs; no run is authoritative until the index sink publishes it.
class InventorySortStorage {
 public:
  virtual ~InventorySortStorage() = default;
  virtual bool reset() = 0;
  virtual bool read(unsigned run, uint64_t offset, std::span<uint8_t> bytes) = 0;
  virtual bool write(unsigned run, uint64_t offset, std::span<const uint8_t> bytes) = 0;
  virtual bool finish(unsigned run, uint64_t size) = 0;
};
// Scratch remains borrowed through consumption and must be disjoint from the
// scanner's hash buffer. Records stay encoded to avoid alignment/lifetime casts.
class InventorySorter final : public SortedInventorySource {
 public:
  InventorySorter(InventorySortStorage& storage, std::span<uint8_t> scratch) : storage(storage), scratch(scratch) {}
  bool build(UnsortedInventorySource& source) {
    ready = false;
    count = cursor = 0;
    active = 0;
    if (scratch.size() < 3 * INVENTORY_INDEX_ENTRY_SIZE || !storage.reset()) return false;
    const size_t capacity = scratch.size() / INVENTORY_INDEX_ENTRY_SIZE;
    size_t buffered = 0;
    ContentManifest manifest;
    for (;;) {
      const auto result = source.next(manifest);
      if (result == InventorySourceResult::Error) return false;
      if (result == InventorySourceResult::End) break;
      if (result != InventorySourceResult::Entry || count == UINT64_MAX / INVENTORY_INDEX_ENTRY_SIZE ||
          !encodeInventoryIndexEntry(manifest, record(scratch, buffered)))
        return false;
      ++count;
      if (++buffered == capacity) {
        if (!flushChunk(buffered)) return false;
        buffered = 0;
      }
    }
    if (buffered && !flushChunk(buffered)) return false;
    if (!storage.finish(0, count * INVENTORY_INDEX_ENTRY_SIZE)) return false;
    uint64_t width = capacity;
    while (width < count) {
      if (!mergePass(width)) return false;
      active ^= 1;
      width = width > count / 2 ? count : width * 2;
    }
    ready = true;
    return true;
  }
  InventorySourceResult next(ContentManifest& manifest) override {
    if (!ready) return InventorySourceResult::Error;
    if (cursor == count) return InventorySourceResult::End;
    auto bytes = scratch.first(INVENTORY_INDEX_ENTRY_SIZE);
    if (!storage.read(active, cursor * INVENTORY_INDEX_ENTRY_SIZE, bytes) ||
        !decodeInventoryIndexEntry(bytes, manifest)) {
      ready = false;
      return InventorySourceResult::Error;
    }
    ++cursor;
    return InventorySourceResult::Entry;
  }

 private:
  static std::span<uint8_t> record(std::span<uint8_t> bytes, size_t index) {
    return bytes.subspan(index * INVENTORY_INDEX_ENTRY_SIZE, INVENTORY_INDEX_ENTRY_SIZE);
  }
  static bool less(std::span<const uint8_t> left, std::span<const uint8_t> right) {
    return std::lexicographical_compare(left.begin() + 2, left.begin() + 34, right.begin() + 2, right.begin() + 34);
  }
  void swap(size_t left, size_t right) {
    auto first = record(scratch, left);
    auto second = record(scratch, right);
    std::swap_ranges(first.begin(), first.end(), second.begin());
  }
  void sift(size_t root, size_t end) {
    while (root < end / 2) {
      size_t child = root * 2 + 1;
      if (child + 1 < end && less(record(scratch, child), record(scratch, child + 1))) ++child;
      if (!less(record(scratch, root), record(scratch, child))) return;
      swap(root, child);
      root = child;
    }
  }
  bool flushChunk(size_t size) {
    for (size_t root = size / 2; root > 0;) sift(--root, size);
    for (size_t end = size; end > 1;) {
      swap(0, --end);
      sift(0, end);
    }
    return storage.write(0, (count - size) * INVENTORY_INDEX_ENTRY_SIZE,
                         scratch.first(size * INVENTORY_INDEX_ENTRY_SIZE));
  }
  struct Bank {
    std::span<uint8_t> bytes;
    uint64_t next = 0, end = 0;
    size_t index = 0, loaded = 0;
  };
  bool load(Bank& bank) {
    if (bank.index < bank.loaded || bank.next == bank.end) return true;
    bank.loaded =
        static_cast<size_t>(std::min<uint64_t>(bank.bytes.size() / INVENTORY_INDEX_ENTRY_SIZE, bank.end - bank.next));
    bank.index = 0;
    auto bytes = bank.bytes.first(bank.loaded * INVENTORY_INDEX_ENTRY_SIZE);
    if (!storage.read(active, bank.next * INVENTORY_INDEX_ENTRY_SIZE, bytes)) return false;
    ContentManifest manifest;
    for (size_t index = 0; index < bank.loaded; ++index) {
      if (!decodeInventoryIndexEntry(record(bytes, index), manifest)) return false;
    }
    bank.next += bank.loaded;
    return true;
  }
  static bool remaining(const Bank& bank) { return bank.index < bank.loaded || bank.next < bank.end; }
  bool mergePass(uint64_t width) {
    const size_t bankSize = scratch.size() / (3 * INVENTORY_INDEX_ENTRY_SIZE) * INVENTORY_INDEX_ENTRY_SIZE;
    auto output = scratch.subspan(2 * bankSize, bankSize);
    uint64_t written = 0;
    size_t buffered = 0;
    for (uint64_t start = 0; start < count;) {
      const uint64_t middle = start + std::min(width, count - start);
      const uint64_t end = middle + std::min(width, count - middle);
      Bank left{scratch.first(bankSize), start, middle};
      Bank right{scratch.subspan(bankSize, bankSize), middle, end};
      while (remaining(left) || remaining(right)) {
        if (!load(left) || !load(right)) return false;
        Bank& selected = !remaining(right) || (remaining(left) &&
                                               !less(record(right.bytes, right.index), record(left.bytes, left.index)))
                             ? left
                             : right;
        std::memcpy(record(output, buffered).data(), record(selected.bytes, selected.index++).data(),
                    INVENTORY_INDEX_ENTRY_SIZE);
        if (++buffered == output.size() / INVENTORY_INDEX_ENTRY_SIZE) {
          if (!storage.write(active ^ 1, written * INVENTORY_INDEX_ENTRY_SIZE, output)) return false;
          written += buffered;
          buffered = 0;
        }
      }
      start = end;
    }
    if (buffered && !storage.write(active ^ 1, written * INVENTORY_INDEX_ENTRY_SIZE,
                                   output.first(buffered * INVENTORY_INDEX_ENTRY_SIZE)))
      return false;
    return storage.finish(active ^ 1, count * INVENTORY_INDEX_ENTRY_SIZE);
  }
  InventorySortStorage& storage;
  std::span<uint8_t> scratch;
  uint64_t count = 0, cursor = 0;
  unsigned active = 0;
  bool ready = false;
};
}  // namespace companion

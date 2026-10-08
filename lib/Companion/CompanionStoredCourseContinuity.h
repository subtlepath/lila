#pragma once

#include <algorithm>

#include "CompanionCourseItemIdentities.h"
#include "CompanionTransfer.h"

namespace companion {
inline bool sameLegacyCourseRecords(TransferStorage& storage, const char* current, const char* candidate,
                                    std::span<uint8_t> scratch, void (*yield)() = nullptr) {
  constexpr uint32_t IDENTITY_TAG = 0x4e454449;
  tinta::core::pack::Header previous{}, next{};
  if (scratch.size() < 2 || !storage.read(current, 0, {reinterpret_cast<uint8_t*>(&previous), sizeof(previous)}) ||
      !storage.read(candidate, 0, {reinterpret_cast<uint8_t*>(&next), sizeof(next)}))
    return false;
  const size_t blockSize = scratch.size() / 2;
  uint32_t nextIndex = 0;
  bool hasIdentity = false;
  for (uint32_t index = 0; index < previous.sectionCount; ++index) {
    tinta::core::pack::DirEntry oldEntry{}, newEntry{};
    if (!storage.read(current, previous.directoryOffset + index * sizeof(oldEntry),
                      {reinterpret_cast<uint8_t*>(&oldEntry), sizeof(oldEntry)}) ||
        oldEntry.tag == IDENTITY_TAG)
      return false;
    do {
      if (nextIndex >= next.sectionCount ||
          !storage.read(candidate, next.directoryOffset + nextIndex++ * sizeof(newEntry),
                        {reinterpret_cast<uint8_t*>(&newEntry), sizeof(newEntry)}))
        return false;
      if (newEntry.tag != IDENTITY_TAG) break;
      if (hasIdentity) return false;
      hasIdentity = true;
    } while (true);
    if (oldEntry.tag != newEntry.tag || oldEntry.size != newEntry.size || oldEntry.count != newEntry.count)
      return false;
    for (uint32_t offset = 0; offset < oldEntry.size;) {
      const size_t count = std::min<size_t>(blockSize, oldEntry.size - offset);
      const auto before = scratch.first(count), after = scratch.subspan(blockSize, count);
      if (!storage.read(current, uint64_t(oldEntry.offset) + offset, before) ||
          !storage.read(candidate, uint64_t(newEntry.offset) + offset, after) ||
          std::memcmp(before.data(), after.data(), count) != 0)
        return false;
      offset += count;
      if (yield) yield();
    }
  }
  while (nextIndex < next.sectionCount) {
    tinta::core::pack::DirEntry entry{};
    if (hasIdentity ||
        !storage.read(candidate, next.directoryOffset + nextIndex++ * sizeof(entry),
                      {reinterpret_cast<uint8_t*>(&entry), sizeof(entry)}) ||
        entry.tag != IDENTITY_TAG)
      return false;
    hasIdentity = true;
  }
  return true;
}
// Call only after complete validation, while installation owns both paths.
inline CourseItemContinuity compareStoredCourseItemIdentities(TransferStorage& storage, const char* currentPath,
                                                              const char* candidatePath, void (*yield)() = nullptr,
                                                              std::span<uint8_t> scratch = {}) {
  class Source final : public tinta::core::pack::PackSource {
   public:
    Source(TransferStorage& storage, const char* path, uint32_t length, void (*yield)(), std::span<uint8_t> cache)
        : storage(storage), path(path), length(length), yield(yield), cache(cache) {}
    uint32_t size() const override { return length; }
    bool read(uint32_t offset, void* output, uint32_t count) override {
      if (offset > length || count > length - offset) return false;
      if (cache.empty()) {
        if (!storage.read(path, offset, {static_cast<uint8_t*>(output), count})) return false;
      } else {
        auto* destination = static_cast<uint8_t*>(output);
        while (count) {
          if (offset < cacheOffset || offset - cacheOffset >= cacheLength) {
            cacheLength = 0;
            cacheOffset = offset;
            const auto available = static_cast<uint32_t>(std::min<size_t>(cache.size(), length - offset));
            if (!storage.read(path, offset, cache.first(available))) return false;
            cacheLength = available;
          }
          const auto available = std::min(count, cacheLength - (offset - cacheOffset));
          std::memcpy(destination, cache.data() + offset - cacheOffset, available);
          destination += available;
          offset += available;
          count -= available;
        }
      }
      if (++readsSinceYield == 32) {
        readsSinceYield = 0;
        if (yield) yield();
      }
      return true;
    }

   private:
    TransferStorage& storage;
    const char* path;
    uint32_t length;
    void (*yield)();
    uint8_t readsSinceYield = 0;
    std::span<uint8_t> cache;
    uint32_t cacheOffset = 0, cacheLength = 0;
  };
  if (!currentPath || !candidatePath) return CourseItemContinuity::InvalidHistory;
  uint64_t currentLength = 0, candidateLength = 0;
  if (storage.stat(currentPath, currentLength) != FileStatus::Present ||
      storage.stat(candidatePath, candidateLength) != FileStatus::Present || currentLength > UINT32_MAX ||
      candidateLength > UINT32_MAX)
    return CourseItemContinuity::InvalidHistory;
  const auto half = scratch.size() / 2;
  Source current(storage, currentPath, static_cast<uint32_t>(currentLength), yield, scratch.first(half));
  Source candidate(storage, candidatePath, static_cast<uint32_t>(candidateLength), yield, scratch.subspan(half));
  return compareCourseItemIdentities(current, candidate);
}
}  // namespace companion

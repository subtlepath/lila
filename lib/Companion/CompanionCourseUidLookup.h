#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "core/pack/Pack.h"

namespace companion {
// The immutable source must first pass complete course validation. A failed read
// invalidates the lookup; absence is reported separately from I/O failure.
class CourseUidLookup final {
 public:
  explicit CourseUidLookup(tinta::core::pack::PackSource& source) : source(source) {}
  bool begin() {
    ready = false;
    items = {};
    uids = {};
    using namespace tinta::core::pack;
    Header header{};
    length = source.size();
    if (!source.read(0, &header, sizeof(header)) ||
        uint64_t(header.directoryOffset) + uint64_t(header.sectionCount) * sizeof(DirEntry) > length)
      return false;
    bool hasItems = false, hasUids = false;
    for (uint16_t i = 0; i < header.sectionCount; ++i) {
      DirEntry entry{};
      if (!source.read(header.directoryOffset + uint32_t(i) * sizeof(entry), &entry, sizeof(entry))) return false;
      if (entry.tag == makeTag("ITEM")) {
        if (hasItems) return false;
        items = entry;
        hasItems = true;
      } else if (entry.tag == makeTag("IUID")) {
        if (hasUids) return false;
        uids = entry;
        hasUids = true;
      }
    }
    ready = hasItems && hasUids && items.count == uids.count && uint64_t(items.count) * sizeof(Item) == items.size &&
            uint64_t(uids.count) * sizeof(ItemUid) == uids.size && uint64_t(items.offset) + items.size <= length &&
            uint64_t(uids.offset) + uids.size <= length && source.size() == length;
    return ready;
  }
  bool valid() const { return ready && source.size() == length; }
  uint32_t count() const { return valid() ? items.count : 0; }
  bool find(uint32_t uid, int32_t& output) {
    if (!ready || source.size() != length || items.count > INT32_MAX) return failure();
    uint32_t first = 0, end = uids.count;
    uint8_t bytes[sizeof(tinta::core::pack::ItemUid)];
    while (first < end) {
      const uint32_t middle = first + (end - first) / 2;
      if (!source.read(uids.offset + middle * sizeof(bytes), bytes, sizeof(bytes))) return failure();
      const uint32_t found = binary_record::getU32(bytes);
      if (found < uid)
        first = middle + 1;
      else if (found > uid)
        end = middle;
      else {
        const uint32_t index = binary_record::getU32(bytes + 4);
        if (index >= items.count || !source.read(items.offset + index * sizeof(tinta::core::pack::Item), bytes, 4) ||
            binary_record::getU32(bytes) != uid || source.size() != length)
          return failure();
        output = static_cast<int32_t>(index);
        return true;
      }
    }
    if (source.size() != length) return failure();
    output = -1;
    return true;
  }

 private:
  tinta::core::pack::PackSource& source;
  tinta::core::pack::DirEntry items{}, uids{};
  uint32_t length = 0;
  bool ready = false;
  bool failure() {
    ready = false;
    return false;
  }
};
}  // namespace companion

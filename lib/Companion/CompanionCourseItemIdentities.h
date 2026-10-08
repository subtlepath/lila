#pragma once

#include <cstring>

#include "core/pack/Pack.h"

namespace companion {
inline bool readCourseItemIdentityTable(tinta::core::pack::PackSource& source, tinta::core::pack::DirEntry& table,
                                        bool& present) {
  constexpr uint32_t TAG = 0x4e454449;  // IDEN
  constexpr uint32_t RECORD_SIZE = 36;
  table = {};
  present = false;
  tinta::core::pack::Header header{};
  if (!source.read(0, &header, sizeof(header))) return false;
  for (uint16_t index = 0; index < header.sectionCount; ++index) {
    tinta::core::pack::DirEntry entry{};
    if (!source.read(header.directoryOffset + uint32_t(index) * sizeof(entry), &entry, sizeof(entry))) return false;
    if (entry.tag != TAG) continue;
    if (present) return false;
    present = true;
    table = entry;
  }
  if (!present) return true;  // Legacy packs require a separate update migration policy.
  if (table.count == UINT32_MAX || uint64_t(table.count) * RECORD_SIZE != table.size ||
      uint64_t(table.offset) + table.size > source.size())
    return false;
  for (uint32_t index = 0; index < table.count; ++index) {
    uint8_t record[RECORD_SIZE];
    if (!source.read(table.offset + index * RECORD_SIZE, record, sizeof(record))) return false;
    const uint32_t uid =
        uint32_t(record[0]) | (uint32_t(record[1]) << 8) | (uint32_t(record[2]) << 16) | (uint32_t(record[3]) << 24);
    if (uid != index + 1) return false;
    uint8_t nonzero = 0;
    for (uint32_t at = 4; at < RECORD_SIZE; ++at) nonzero |= record[at];
    if (!nonzero) return false;
  }
  return true;
}
inline bool validCourseItemIdentities(tinta::core::pack::Pack& pack, tinta::core::pack::PackSource& source) {
  tinta::core::pack::DirEntry table{};
  bool present = false;
  if (!readCourseItemIdentityTable(source, table, present)) return false;
  if (!present) return true;
  for (uint32_t index = 0; index < pack.itemCount(); ++index) {
    const auto uid = pack.uidAt(index);
    if (uid == 0 || uid > table.count) return false;
  }
  return true;
}

enum class CourseItemContinuity { Compatible, MissingHistory, InvalidHistory, RemovedHistory, ReassignedIdentity };
// Sources must remain immutable and have passed complete course validation.
inline CourseItemContinuity compareCourseItemIdentities(tinta::core::pack::PackSource& current,
                                                        tinta::core::pack::PackSource& candidate) {
  tinta::core::pack::DirEntry previous{}, next{};
  bool hasPrevious = false, hasNext = false;
  if (!readCourseItemIdentityTable(current, previous, hasPrevious) ||
      !readCourseItemIdentityTable(candidate, next, hasNext))
    return CourseItemContinuity::InvalidHistory;
  if (!hasPrevious || !hasNext) return CourseItemContinuity::MissingHistory;
  if (next.count < previous.count) return CourseItemContinuity::RemovedHistory;
  for (uint32_t index = 0; index < previous.count; ++index) {
    uint8_t oldRecord[36], newRecord[36];
    if (!current.read(previous.offset + index * 36, oldRecord, sizeof(oldRecord)) ||
        !candidate.read(next.offset + index * 36, newRecord, sizeof(newRecord)))
      return CourseItemContinuity::InvalidHistory;
    if (std::memcmp(oldRecord, newRecord, sizeof(oldRecord)) != 0) return CourseItemContinuity::ReassignedIdentity;
  }
  return CourseItemContinuity::Compatible;
}
}  // namespace companion

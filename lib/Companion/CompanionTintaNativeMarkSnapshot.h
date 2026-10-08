#pragma once

#include <algorithm>
#include <array>
#include <span>

#include "../Serialization/BinaryRecordBytes.h"
#include "core/library/MarkLog.h"

namespace companion {
enum class TintaNativeMarkSnapshotResult { Ok, Invalid, Unresolved, Duplicate, IoError };
inline constexpr size_t TINTA_NATIVE_MARK_SNAPSHOT_SIZE =
    tinta::core::library::MarkLog::kHeaderSize +
    tinta::core::library::MarkLog::kCapacity * tinta::core::library::MarkLog::kRecordSize;

// Caller binds the verified completion set to its course/frontier and owns the scratch buffer.
inline TintaNativeMarkSnapshotResult restoreTintaNativeMarks(tinta::core::StateStore& store, const char* name,
                                                             uint32_t count, void* context,
                                                             bool (*identityAt)(void*, uint32_t, uint32_t&),
                                                             bool (*legacyKey)(void*, uint32_t, uint32_t&),
                                                             std::span<uint8_t> scratch) {
  using Log = tinta::core::library::MarkLog;
  if (!name || !name[0] || !identityAt || !legacyKey || count > Log::kCapacity ||
      scratch.size() < Log::kHeaderSize + count * Log::kRecordSize)
    return TintaNativeMarkSnapshotResult::Invalid;
  const size_t length = Log::kHeaderSize + count * Log::kRecordSize;
  std::fill_n(scratch.begin(), length, 0);
  scratch[0] = 'T';
  scratch[1] = 'M';
  scratch[2] = 'K';
  scratch[3] = '1';
  for (uint32_t at = 0; at < count; ++at) {
    uint32_t identity = 0, key = 0;
    if (!identityAt(context, at, identity)) return TintaNativeMarkSnapshotResult::IoError;
    if (!identity || identity == UINT32_MAX) return TintaNativeMarkSnapshotResult::Invalid;
    if (!legacyKey(context, identity, key)) return TintaNativeMarkSnapshotResult::Unresolved;
    if (!key) return TintaNativeMarkSnapshotResult::Invalid;
    for (uint32_t previous = 0; previous < at; ++previous)
      if (binary_record::getU32(scratch.data() + Log::kHeaderSize + previous * Log::kRecordSize) == key)
        return TintaNativeMarkSnapshotResult::Duplicate;
    auto* record = scratch.data() + Log::kHeaderSize + at * Log::kRecordSize;
    binary_record::putU32(record, key);
    record[4] = 1;
    binary_record::putU16(record + 6, static_cast<uint16_t>(binary_record::crc32(record, 6)));
  }
  if (!store.available()) return TintaNativeMarkSnapshotResult::IoError;
  if (store.size(name) == static_cast<int32_t>(length)) {
    std::array<uint8_t, 64> previous{};
    bool identical = true;
    for (size_t offset = 0; offset < length; offset += previous.size()) {
      const size_t amount = std::min(previous.size(), length - offset);
      if (store.read(name, offset, previous.data(), amount) != static_cast<int32_t>(amount))
        return TintaNativeMarkSnapshotResult::IoError;
      if (!std::equal(previous.begin(), previous.begin() + amount, scratch.begin() + offset)) identical = false;
    }
    if (identical) return TintaNativeMarkSnapshotResult::Ok;
  }
  // StateStore replacement preserves the previous file until the complete snapshot is durable.
  if (!store.replace(name, scratch.data(), length)) return TintaNativeMarkSnapshotResult::IoError;
  return TintaNativeMarkSnapshotResult::Ok;
}
}  // namespace companion

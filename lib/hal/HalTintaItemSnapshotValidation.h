#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "../Serialization/BinaryRecordBytes.h"
#include "core/srs/ItemState.h"

namespace companion {
// Validates canonical companion output, not general recoverable local items.bin files.
inline bool validateTintaItemSnapshot(HalFile& file, uint16_t studyDay, std::span<uint8_t> scratch,
                                      bool (*progress)(void*) = nullptr, void* context = nullptr) {
  const auto failure = [](const char* reason) {
    LOG_ERR("COMPANION", "Tinta item snapshot validation failed: %s", reason);
    return false;
  };
  if (scratch.size() < 80 || !file.isOpen() || file.isDirectory()) return failure("arguments");
  const uint64_t length = file.fileSize64();
  uint32_t count = 0;
  uint16_t newItems = 0, reviews = 0;
  for (unsigned slot = 0; slot < 2; ++slot) {
    if (progress && !progress(context)) return failure("cancelled");
    if (!file.seek64(slot * 512) || file.read(scratch.data(), 80) != 80) return failure("header read");
    const auto* bytes = scratch.data();
    if (bytes[0] != 'T' || bytes[1] != 'I' || bytes[2] != 'S' || bytes[3] != '1' ||
        binary_record::getU16(bytes + 4) != 1 || binary_record::getU16(bytes + 6) != 80 ||
        binary_record::getU32(bytes + 8) != slot + 1 || binary_record::getU32(bytes + 16) != 0 ||
        binary_record::getU16(bytes + 20) != studyDay ||
        binary_record::getU32(bytes + 76) != binary_record::crc32(bytes, 76) ||
        std::any_of(bytes + 26, bytes + 76, [](uint8_t byte) { return byte != 0; }))
      return failure("header");
    const uint32_t records = binary_record::getU32(bytes + 12);
    if (records > 32767 || length != 1024 + static_cast<uint64_t>(records) * 16) return failure("extent");
    if (!slot) {
      count = records;
      newItems = binary_record::getU16(bytes + 22);
      reviews = binary_record::getU16(bytes + 24);
    } else if (count != records || newItems != binary_record::getU16(bytes + 22) ||
               reviews != binary_record::getU16(bytes + 24))
      return failure("header disagreement");
  }
  if (!file.seek64(1024)) return failure("records seek");
  uint32_t previous = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (i % 32 == 0 && progress && !progress(context)) return failure("cancelled");
    tinta::core::ItemState item;
    if (file.read(scratch.data(), 16) != 16 || !tinta::core::ItemState::decode(scratch.data(), item) ||
        item.uid <= previous || item.uid == UINT32_MAX)
      return failure("item");
    previous = item.uid;
  }
  if (file.fileSize64() != length) return failure("changed extent");
  return true;
}
}  // namespace companion

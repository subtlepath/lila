#pragma once

#include <cstring>

#include "core/srs/Bytes.h"
#include "core/srs/ItemState.h"

namespace tinta::core {

// Read-only codec shared by local recovery and companion compatibility inspection.
struct ProgressHeader {
  uint32_t seq = 0;
  uint32_t recordCount = 0;
  uint32_t journalCount = 0;
  DayNumber statDay = 0;
  uint16_t statNew = 0;
  uint16_t statReviews = 0;
  bool pendingValid = false;
  bool undoValid = false;
  uint32_t pendingSlot = 0;
  ItemState pending;
  uint32_t undoSlot = 0;
  ItemState undoBefore;
  DayNumber undoStatDay = 0;
  uint16_t undoStatNew = 0;
  uint16_t undoStatReviews = 0;

  static constexpr uint32_t PACKED_SIZE = 80;
  static constexpr uint32_t MAX_RECORDS = 0x7FFF;

  static bool decode(const uint8_t in[PACKED_SIZE], ProgressHeader& out) {
    if (std::memcmp(in, "TIS1", 4) != 0) return false;
    if (getU16(in + 4) != 1 || getU16(in + 6) != PACKED_SIZE) return false;
    if (getU32(in + 76) != crc32(in, 76)) return false;
    ProgressHeader h;
    h.seq = getU32(in + 8);
    h.recordCount = getU32(in + 12);
    h.journalCount = getU32(in + 16);
    h.statDay = getU16(in + 20);
    h.statNew = getU16(in + 22);
    h.statReviews = getU16(in + 24);
    const uint16_t flags = getU16(in + 26);
    h.pendingValid = (flags & 1u) != 0;
    h.undoValid = (flags & 2u) != 0;
    h.pendingSlot = getU32(in + 28);
    if (h.pendingValid && !ItemState::decode(in + 32, h.pending)) return false;
    h.undoSlot = getU32(in + 48);
    if (h.undoValid && !ItemState::decode(in + 52, h.undoBefore)) return false;
    h.undoStatDay = getU16(in + 68);
    h.undoStatNew = getU16(in + 70);
    h.undoStatReviews = getU16(in + 72);
    if (h.recordCount > MAX_RECORDS) return false;
    if (h.pendingValid && h.pendingSlot >= h.recordCount) return false;
    if (h.undoValid && h.undoSlot >= h.recordCount) return false;
    out = h;
    return true;
  }
};

}  // namespace tinta::core

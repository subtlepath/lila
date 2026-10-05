#include "core/srs/ItemState.h"

#include "core/srs/Bytes.h"

namespace tinta::core {

void ItemState::setStabilityDays(float days) {
  const float scaled = days * kStabilityScale + 0.5f;
  // Written to be false for NaN, which then takes the floor.
  if (!(scaled >= 1.0f)) {
    stability = 1;
  } else if (scaled >= static_cast<float>(kMaxStabilityRaw)) {
    stability = kMaxStabilityRaw;
  } else {
    stability = static_cast<uint16_t>(scaled);
  }
}

void ItemState::setDifficultyValue(float d) {
  const float scaled = d * kDifficultyScale + 0.5f;
  if (!(scaled >= static_cast<float>(kMinDifficultyRaw))) {
    difficulty = kMinDifficultyRaw;
  } else if (scaled >= static_cast<float>(kMaxDifficultyRaw)) {
    difficulty = kMaxDifficultyRaw;
  } else {
    difficulty = static_cast<uint8_t>(scaled);
  }
}

void ItemState::encode(uint8_t out[kPackedSize]) const {
  putU32(out + 0, uid);
  putU16(out + 4, dueDay);
  putU16(out + 6, lastDay);
  putU16(out + 8, stability);
  out[10] = difficulty;
  out[11] = phase;
  out[12] = reps;
  out[13] = lapses;
  putU16(out + 14, flags);
}

bool ItemState::decode(const uint8_t in[kPackedSize], ItemState& out) {
  ItemState s;
  s.uid = getU32(in + 0);
  s.dueDay = getU16(in + 4);
  s.lastDay = getU16(in + 6);
  s.stability = getU16(in + 8);
  s.difficulty = in[10];
  s.phase = in[11];
  s.reps = in[12];
  s.lapses = in[13];
  s.flags = getU16(in + 14);

  if ((s.phase & 0xE0) != 0 || (s.flags & ~item_flag::kAll) != 0) return false;
  if (s.isNew()) {
    if (s.stability != 0 || s.difficulty != 0 || s.step() != 0) return false;
  } else {
    if (s.stability == 0) return false;
    if (s.difficulty < kMinDifficultyRaw || s.difficulty > kMaxDifficultyRaw) return false;
    if (s.reps == 0) return false;
  }
  out = s;
  return true;
}

}  // namespace tinta::core

#pragma once

#include <cstdint>

#include "core/Clock.h"

namespace tinta::core {

enum class Phase : uint8_t {
  New = 0,         // never graded (or its first grade was undone)
  Learning = 1,    // first seen, working through the in-session steps
  Review = 2,      // graduated: day-based scheduling
  Relearning = 3,  // lapsed, working through the in-session steps again
};

namespace item_flag {
constexpr uint16_t kSuspended = 1u << 0;
constexpr uint16_t kLeech = 1u << 1;
constexpr uint16_t kStarred = 1u << 2;
constexpr uint16_t kAll = kSuspended | kLeech | kStarred;
}  // namespace item_flag

// One item's learning state: the 16-byte record of items.bin (PLAN.md 8.3).
// Fields hold the stored, fixed-point values; the float accessors convert.
// encode()/decode() define the on-disk layout byte by byte, little-endian:
//
//   0 uid u32        4 dueDay u16      6 lastDay u16     8 stability u16
//  10 difficulty u8 11 phase u8       12 reps u8        13 lapses u8
//  14 flags u16
//
// stability   days × 16, 1..65535 (0.0625 to 4095.9 days) once graded.
//             FSRS's own minimum is 0.001; anything below 1/16 day schedules
//             identically (a one-day interval), so the floor costs nothing.
// difficulty  D × 25, 25..250 for D in 1..10 once graded (0.04 steps).
// phase       bits 0-1 Phase, bits 2-4 learning step, bits 5-7 zero.
// flags       item_flag bits, others zero.
struct ItemState {
  static constexpr uint32_t kPackedSize = 16;
  static constexpr float kStabilityScale = 16.0f;
  static constexpr float kDifficultyScale = 25.0f;
  static constexpr uint16_t kMaxStabilityRaw = 0xFFFF;
  static constexpr uint8_t kMinDifficultyRaw = 25;
  static constexpr uint8_t kMaxDifficultyRaw = 250;
  static constexpr uint8_t kMaxStep = 7;

  uint32_t uid = 0;
  DayNumber dueDay = 0;
  DayNumber lastDay = 0;
  uint16_t stability = 0;
  uint8_t difficulty = 0;
  uint8_t phase = 0;
  uint8_t reps = 0;
  uint8_t lapses = 0;
  uint16_t flags = 0;

  static ItemState fresh(uint32_t uid) {
    ItemState s;
    s.uid = uid;
    return s;
  }

  Phase phaseKind() const { return static_cast<Phase>(phase & 0x03); }
  uint8_t step() const { return static_cast<uint8_t>((phase >> 2) & 0x07); }
  void setPhase(Phase p, uint8_t step = 0) {
    if (step > kMaxStep) step = kMaxStep;
    phase = static_cast<uint8_t>(static_cast<uint8_t>(p) | (step << 2));
  }
  bool isNew() const { return phaseKind() == Phase::New; }
  bool inSteps() const { return phaseKind() == Phase::Learning || phaseKind() == Phase::Relearning; }
  bool suspended() const { return (flags & item_flag::kSuspended) != 0; }

  float stabilityDays() const { return static_cast<float>(stability) / kStabilityScale; }
  float difficultyValue() const { return static_cast<float>(difficulty) / kDifficultyScale; }
  // Round to the stored precision and clamp to the stored range.
  void setStabilityDays(float days);
  void setDifficultyValue(float d);

  void encode(uint8_t out[kPackedSize]) const;
  // False if the bytes are not a record this firmware could have written.
  static bool decode(const uint8_t in[kPackedSize], ItemState& out);

  bool operator==(const ItemState& o) const {
    return uid == o.uid && dueDay == o.dueDay && lastDay == o.lastDay && stability == o.stability &&
           difficulty == o.difficulty && phase == o.phase && reps == o.reps && lapses == o.lapses && flags == o.flags;
  }
  bool operator!=(const ItemState& o) const { return !(*this == o); }
};

}  // namespace tinta::core

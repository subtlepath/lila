#pragma once

#include "CompanionTintaBody.h"
#include "core/srs/ItemState.h"

namespace companion {
enum class LegacyTintaStarPlanResult { Record, End, Unavailable, Invalid, IoError };
// Parent supplies hash-verified frozen membership and an exclusive, complete
// disposable review projection. Bodies require durable identities and causal
// ordering after the converted reviews before journal publication/replay.
class LegacyTintaStarPlan final {
 public:
  static constexpr size_t CAPACITY = 96;
  using NextItem = bool (*)(void*, bool, uint32_t, tinta::core::ItemState&, bool&);
  using NextMember = LegacyTintaStarPlanResult (*)(void*, uint32_t&);
  using Permission = bool (*)(void*);
  LegacyTintaStarPlan(NextItem nextItem, Permission permitted, void* context)
      : nextItem(nextItem), permitted(permitted), context(context) {}
  // Source must return End only after verifying its full frozen membership.
  bool beginFromSource(const Identity& inputCourse, NextMember source) {
    if (operating) return false;
    ready = complete = false;
    if (!nextItem || !permitted || !source || !tinta_body_detail::nonzero(inputCourse) ||
        overlaps(&inputCourse, sizeof(inputCourse)))
      return false;
    course = inputCourse;
    count = index = 0;
    previous = 0;
    hasPrevious = additions = false;
    operating = true;
    cancelled = false;
    bool valid = guard();
    while (valid) {
      uint32_t uid = 0;
      const auto result = source(context, uid);
      valid = guard();
      if (valid && result == LegacyTintaStarPlanResult::End) break;
      valid = valid && result == LegacyTintaStarPlanResult::Record && count < CAPACITY && uid && uid != UINT32_MAX;
      for (uint16_t at = 0; valid && at < count; ++at) valid = members[at] != uid;
      if (valid) members[count++] = uid;
    }
    ready = valid && guard();
    operating = false;
    return ready;
  }
  bool begin(const Identity& inputCourse, std::span<const uint32_t> membership) {
    if (operating) return false;
    ready = complete = false;
    if (!nextItem || !permitted || !tinta_body_detail::nonzero(inputCourse) || membership.size() > CAPACITY ||
        overlaps(&inputCourse, sizeof(inputCourse)) || overlaps(membership.data(), membership.size_bytes()))
      return false;
    course = inputCourse;
    count = static_cast<uint16_t>(membership.size());
    for (uint16_t at = 0; at < count; ++at) {
      const uint32_t uid = membership[at];
      if (!uid || uid == UINT32_MAX) return false;
      for (uint16_t prior = 0; prior < at; ++prior)
        if (members[prior] == uid) return false;
      members[at] = uid;
    }
    previous = 0;
    hasPrevious = additions = false;
    index = 0;
    operating = true;
    cancelled = false;
    ready = guard();
    operating = false;
    return ready;
  }
  LegacyTintaStarPlanResult next(TintaBody& output) {
    if (operating || !ready || overlaps(&output, sizeof(output))) return LegacyTintaStarPlanResult::Unavailable;
    operating = true;
    if (!guard()) return finish(LegacyTintaStarPlanResult::Unavailable);
    if (complete) return finish(LegacyTintaStarPlanResult::End);
    while (!additions) {
      tinta::core::ItemState item;
      bool found = false;
      if (!nextItem(context, hasPrevious, previous, item, found)) return finish(LegacyTintaStarPlanResult::IoError);
      if (!guard()) return finish(LegacyTintaStarPlanResult::Unavailable);
      if (!found) {
        additions = true;
        break;
      }
      if (!item.uid || item.uid == UINT32_MAX || (hasPrevious && item.uid <= previous) ||
          (item.flags & ~tinta::core::item_flag::kAll))
        return finish(LegacyTintaStarPlanResult::Invalid);
      previous = item.uid;
      hasPrevious = true;
      if ((item.flags & tinta::core::item_flag::kStarred) && !contains(item.uid)) return emit(item.uid, false, output);
    }
    if (index < count) return emit(members[index++], true, output);
    complete = true;
    return finish(LegacyTintaStarPlanResult::End);
  }
  bool completed() const { return !operating && ready && complete; }
  void close() {
    if (operating) cancelled = true;
    ready = complete = false;
  }

 private:
  NextItem nextItem;
  Permission permitted;
  void* context;
  Identity course{};
  // Fixed snapshot survives workspace reuse after the frozen mark reader ends.
  std::array<uint32_t, CAPACITY> members{};
  uint32_t previous = 0;
  uint16_t count = 0, index = 0;
  bool operating = false, ready = false, complete = false, cancelled = false, hasPrevious = false, additions = false;
  bool overlaps(const void* input, size_t size) const {
    const auto start = reinterpret_cast<uintptr_t>(input), owner = reinterpret_cast<uintptr_t>(this);
    return start <= owner ? owner - start < size : start - owner < sizeof(*this);
  }
  bool contains(uint32_t uid) const {
    for (uint16_t at = 0; at < count; ++at)
      if (members[at] == uid) return true;
    return false;
  }
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled; }
  LegacyTintaStarPlanResult finish(LegacyTintaStarPlanResult result) {
    if (!guard()) result = LegacyTintaStarPlanResult::Unavailable;
    if (result != LegacyTintaStarPlanResult::Record && result != LegacyTintaStarPlanResult::End)
      ready = complete = false;
    operating = false;
    return result;
  }
  LegacyTintaStarPlanResult emit(uint32_t uid, bool enabled, TintaBody& output) {
    TintaBody body;
    body.course = course;
    body.uid = uid;
    body.kind = EventKind::Star;
    body.enabled = enabled;
    const auto result = finish(LegacyTintaStarPlanResult::Record);
    if (result == LegacyTintaStarPlanResult::Record) output = body;
    return result;
  }
};
}  // namespace companion

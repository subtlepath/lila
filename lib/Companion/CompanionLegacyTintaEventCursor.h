#pragma once

#include <array>

#include "CompanionLegacyTintaJournal.h"

namespace companion {
struct LegacyTintaEventIdentities {
  std::array<EventIdentity, 2> events{};
  EventIdentity undoTarget{};
  uint8_t count = 0;
  bool operator==(const LegacyTintaEventIdentities&) const = default;
};
// The owner durably reserves a dedicated migration origin/epoch and verifies the
// frozen stream and replay. Restarting this cursor with that reservation is
// deterministic; it neither reserves identities nor proves shared ancestry.
class LegacyTintaEventCursor final {
 public:
  bool begin(const EventIdentity& first, uint32_t records) {
    ready = false;
    if (!tinta_body_detail::validIdentity(first) || first.sequence != 1 ||
        records > LegacyTintaJournalDecoder::MAX_BYTES / LegacyTintaJournalDecoder::RECORD_SIZE)
      return false;
    next = first;
    expected = records;
    index = lastReviewIndex = lastReviewUid = 0;
    lastReview = {};
    undoAvailable = false;
    ready = true;
    return true;
  }
  bool assign(uint32_t recordIndex, const LegacyTintaEntry& entry, LegacyTintaEventIdentities& output) {
    if (!ready || index >= expected || recordIndex != index || !entry.uid || entry.uid == UINT32_MAX ||
        overlaps(&output, sizeof(output), this, sizeof(*this)) ||
        overlaps(&output, sizeof(output), &entry, sizeof(entry)))
      return false;
    uint8_t count = 1;
    switch (entry.operation) {
      case LegacyTintaOperation::Review:
        if (entry.grade < 1 || entry.grade > 4 || entry.format > 9) return false;
        break;
      case LegacyTintaOperation::Undo:
        if (!undoAvailable || entry.uid != lastReviewUid || entry.undoRecord != lastReviewIndex) return false;
        break;
      case LegacyTintaOperation::Flags:
        if (entry.flags & ~7U) return false;
        count = 2;
        break;
      default:
        return false;
    }
    if (next.sequence > UINT64_MAX - count) return false;
    LegacyTintaEventIdentities result;
    result.count = count;
    result.events[0] = next;
    if (count == 2) {
      result.events[1] = next;
      ++result.events[1].sequence;
    }
    if (entry.operation == LegacyTintaOperation::Undo) result.undoTarget = lastReview;
    undoAvailable = entry.operation == LegacyTintaOperation::Review;
    if (undoAvailable) {
      lastReview = next;
      lastReviewIndex = index;
      lastReviewUid = entry.uid;
    }
    next.sequence += count;
    ++index;
    output = result;
    return true;
  }
  bool complete() const { return ready && index == expected; }
  uint32_t records() const { return index; }
  uint64_t events() const { return ready ? next.sequence - 1 : 0; }

 private:
  EventIdentity next{}, lastReview{};
  uint32_t expected = 0, index = 0, lastReviewIndex = 0, lastReviewUid = 0;
  bool ready = false, undoAvailable = false;
  static bool overlaps(const void* a, size_t aSize, const void* b, size_t bSize) {
    const auto x = reinterpret_cast<uintptr_t>(a), y = reinterpret_cast<uintptr_t>(b);
    return x <= y ? y - x < aSize : x - y < bSize;
  }
};
}  // namespace companion

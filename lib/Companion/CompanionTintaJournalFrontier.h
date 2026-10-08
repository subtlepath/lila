#pragma once

#include <algorithm>

#include "CompanionRecords.h"

namespace companion {
// Caller validates complete journal/replay and supplies unique events in identity order.
class TintaJournalFrontierEncoding {
 public:
  using Sink = bool (*)(void*, std::span<const uint8_t>);
  TintaJournalFrontierEncoding(std::span<uint8_t> scratch, void* context, Sink sink)
      : scratch(scratch), context(context), sink(sink) {}

  bool begin(uint64_t count) {
    ready = false;
    written = 0;
    expected = count;
    if (!sink || scratch.size() < MAX_RECORD_SIZE) return false;
    constexpr std::array<uint8_t, 4> magic{'T', 'J', 'F', '1'};
    std::copy(magic.begin(), magic.end(), scratch.begin());
    for (unsigned i = 0; i < 8; ++i) scratch[4 + i] = static_cast<uint8_t>(count >> (8 * i));
    ready = sink(context, scratch.first(12));
    return ready;
  }

  bool append(const SyncEvent& event) {
    if (!ready) return false;
    const auto& identity = event.identity;
    if (written >= expected || event.kind < EventKind::ReadingPosition || event.kind > EventKind::ReadingComplete ||
        (written && !before(previous, identity)))
      return fail();
    const auto length = encodeRecord(event, scratch);
    if (!length) return fail();
    const std::array<uint8_t, 2> size{static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8)};
    if (!sink(context, size) || !sink(context, scratch.first(length))) return fail();
    previous = identity;
    ++written;
    return true;
  }

  bool complete() const { return ready && written == expected; }

 private:
  static bool before(const EventIdentity& a, const EventIdentity& b) {
    if (a.origin != b.origin)
      return std::lexicographical_compare(a.origin.begin(), a.origin.end(), b.origin.begin(), b.origin.end());
    if (a.epoch != b.epoch) return a.epoch < b.epoch;
    return a.sequence < b.sequence;
  }
  bool fail() {
    ready = false;
    return false;
  }
  std::span<uint8_t> scratch;
  void* context;
  Sink sink;
  EventIdentity previous{};
  uint64_t expected = 0, written = 0;
  bool ready = false;
};
}  // namespace companion

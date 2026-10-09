#pragma once

#include <cstdint>
#include <optional>

class ProgressSaveDebounce final {
 public:
  static constexpr uint32_t INTERVAL_MS = 5000;
  struct Position {
    int spine, page, count;
    std::optional<uint32_t> offset;
    bool operator==(const Position&) const = default;
  };
  void stage(const Position& position, bool force = false) {
    if (!force && saved && *saved == position)
      pending.reset();
    else
      pending = position;
  }
  bool due(uint32_t now) const { return pending && (!attempted || now - lastAttempt >= INTERVAL_MS); }
  bool hasPending() const { return pending.has_value(); }
  const Position& position() const { return *pending; }
  void complete(uint32_t now, bool success) {
    attempted = true;
    lastAttempt = now;
    if (success) {
      saved = pending;
      pending.reset();
    } else {
      // A failed save may have committed before acknowledgement.
      saved.reset();
    }
  }
  uint32_t lastAttemptMs() const { return lastAttempt; }
  void clearPending() { pending.reset(); }

 private:
  std::optional<Position> pending, saved;
  uint32_t lastAttempt = 0;
  bool attempted = false;
};
static_assert(sizeof(ProgressSaveDebounce) <= 64);

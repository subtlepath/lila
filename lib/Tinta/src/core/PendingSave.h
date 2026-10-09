#pragma once

#include <cstdint>

namespace tinta::core {
class PendingSave {
 public:
  static constexpr uint32_t kRetryMs = 1000;
  explicit PendingSave(bool pending = false) : dirty_(pending) {}
  void mark() { dirty_ = true; }
  bool pending() const { return dirty_; }
  bool due(uint32_t now) const { return dirty_ && (!attempted_ || now - lastAttempt_ >= kRetryMs); }
  void complete(uint32_t now, bool saved) {
    lastAttempt_ = now;
    attempted_ = true;
    dirty_ = !saved;
  }

 private:
  uint32_t lastAttempt_ = 0;
  bool dirty_ = false;
  bool attempted_ = false;
};
static_assert(sizeof(PendingSave) <= 8);
}  // namespace tinta::core

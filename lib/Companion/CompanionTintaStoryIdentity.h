#pragma once

#include <cstdint>
#include <span>

namespace companion {
// Caller supplies the complete title bytes, excluding the terminating NUL.
class TintaStoryIdentity {
 public:
  bool begin(uint8_t kind, uint32_t lessonIdentity) {
    valid = kind <= 1;
    hash = 2166136261u;
    if (!valid) return false;
    constexpr uint8_t prefix[] = {'T', 'S', 'T', '1'};
    for (const auto byte : prefix) append(byte);
    append(kind);
    for (unsigned i = 0; i < 4; ++i) append(static_cast<uint8_t>(lessonIdentity >> (i * 8)));
    return true;
  }
  bool title(std::span<const uint8_t> bytes) {
    if (!valid) return false;
    for (const auto byte : bytes) {
      if (!byte) {
        valid = false;
        return false;
      }
      append(byte);
    }
    return true;
  }
  bool finish(uint32_t& identity) const {
    if (!valid || hash == UINT32_MAX) return false;
    identity = hash ? hash : 1;
    return true;
  }

 private:
  void append(uint8_t byte) { hash = (hash ^ byte) * 16777619u; }
  uint32_t hash = 2166136261u;
  bool valid = false;
};
}  // namespace companion

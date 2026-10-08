#pragma once
#include <CompanionPairings.h>
namespace companion {
class HalPairingsStorage final : public PairingsStorage {
 public:
  PairingsRead read(std::span<uint8_t> bytes) override;
  bool write(std::span<const uint8_t> bytes) override;

 private:
  // Readback exceeds the per-function local budget alongside Preferences.
  std::array<uint8_t, PAIRINGS_RECORD_SIZE> verified{};
};
}  // namespace companion

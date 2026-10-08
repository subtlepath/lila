#pragma once

#include "CompanionIdentity.h"

namespace companion {
class HalIdentityStorage final : public IdentityStorage {
 public:
  bool hardwareIdentity(Identity& output) override;
  bool cardIdentity(Identity& output) override;
  IdentityRead readBinding(std::span<uint8_t> bytes) override;
  bool writeBinding(std::span<const uint8_t> bytes) override;
  IdentityRead readMarker(Identity& output) override;
  bool createMarker(const Identity& value) override;
  bool randomIdentity(Identity& output) override;
};
}  // namespace companion

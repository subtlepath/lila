#pragma once

#include <span>

#include "CompanionRecords.h"

namespace companion {
inline constexpr size_t IDENTITY_RECORD_SIZE = 80;
inline constexpr char CARD_MARKER_PATH[] = "/.crosspoint/companion/card-id";

enum class IdentityRead : uint8_t { Present, Missing, Corrupt, Error };
class IdentityStorage {
 public:
  virtual ~IdentityStorage() = default;
  virtual bool hardwareIdentity(Identity& output) = 0;
  virtual bool cardIdentity(Identity& output) = 0;
  virtual IdentityRead readBinding(std::span<uint8_t> bytes) = 0;
  virtual bool writeBinding(std::span<const uint8_t> bytes) = 0;
  virtual IdentityRead readMarker(Identity& output) = 0;
  virtual bool createMarker(const Identity& value) = 0;
  virtual bool randomIdentity(Identity& output) = 0;
};
struct IdentityState {
  Identity device{};
  Identity storageGeneration{};
  uint64_t eventEpoch = 0;
  bool operator==(const IdentityState&) const = default;
};
enum class IdentityResult : uint8_t { Ok, IoError, Corrupt, WrongHardware, Exhausted, EntropyError };

// Call once per reading/sync lifecycle, before generating events. Each successful
// call durably reserves a fresh epoch; sequence numbers may start at one in it.
IdentityResult provisionIdentity(IdentityStorage& storage, IdentityState& output);
}  // namespace companion

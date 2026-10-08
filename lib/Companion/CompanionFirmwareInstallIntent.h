#pragma once

#include "CompanionCourseBinding.h"
#include "CompanionFirmwareInstallRequest.h"

namespace companion {
inline constexpr char FIRMWARE_INSTALL_INTENT_PATH[] = "/.crosspoint/companion/firmware-install";
inline constexpr char FIRMWARE_INSTALL_INTENT_STAGE[] = "/.crosspoint/companion/firmware-install-next";
inline constexpr size_t FIRMWARE_INSTALL_AUTHORIZATION_SIZE = FIRMWARE_INSTALL_REQUEST_SIZE + 16;
inline constexpr size_t FIRMWARE_INSTALL_INTENT_SIZE = FIRMWARE_INSTALL_AUTHORIZATION_SIZE + 4;
struct FirmwareInstallAuthorization {
  FirmwareInstallRequest request;
  Identity owner{};
  bool operator==(const FirmwareInstallAuthorization&) const = default;
};
inline bool validFirmwareInstallAuthorization(const FirmwareInstallAuthorization& value) {
  return validFirmwareInstallRequest(value.request) &&
         std::any_of(value.owner.begin(), value.owner.end(), [](uint8_t byte) { return byte != 0; });
}
inline bool encodeFirmwareInstallAuthorization(const FirmwareInstallAuthorization& value, std::span<uint8_t> bytes) {
  if (bytes.size() != FIRMWARE_INSTALL_AUTHORIZATION_SIZE || !validFirmwareInstallAuthorization(value)) return false;
  if (!encodeFirmwareInstallRequest(value.request, bytes.first(FIRMWARE_INSTALL_REQUEST_SIZE))) return false;
  std::copy(value.owner.begin(), value.owner.end(), bytes.begin() + FIRMWARE_INSTALL_REQUEST_SIZE);
  return true;
}
inline bool decodeFirmwareInstallAuthorization(std::span<const uint8_t> bytes, FirmwareInstallAuthorization& output) {
  if (bytes.size() != FIRMWARE_INSTALL_AUTHORIZATION_SIZE) return false;
  FirmwareInstallAuthorization value;
  if (!decodeFirmwareInstallRequest(bytes.first(FIRMWARE_INSTALL_REQUEST_SIZE), value.request)) return false;
  std::copy_n(bytes.begin() + FIRMWARE_INSTALL_REQUEST_SIZE, 16, value.owner.begin());
  if (!validFirmwareInstallAuthorization(value)) return false;
  output = value;
  return true;
}
// A successful content commit or flash write is insufficient; the running-image digest is authoritative.
inline bool firmwareInstallationBootVerified(const FirmwareInstallAuthorization& authorization,
                                             const FirmwareReaderInfo& running) {
  return validFirmwareInstallAuthorization(authorization) && validFirmwareReaderInfo(running) &&
         running.generation == authorization.request.generation && running.board == authorization.request.board &&
         running.chip == authorization.request.chip && running.runningBuild == authorization.request.hash;
}
enum class FirmwareInstallIntentResult { Ok, Missing, Invalid, Conflict, Corrupt, IoError };

// Caller excludes storage writers. Scratch is borrowed, at least 124 bytes,
// and disjoint from request/output objects. No heap allocation is required.
class FirmwareInstallIntent {
 public:
  FirmwareInstallIntent(TransferStorage& storage, std::span<uint8_t> scratch) : storage(storage), scratch(scratch) {}
  FirmwareInstallIntentResult load(FirmwareInstallAuthorization& output) {
    return read(FIRMWARE_INSTALL_INTENT_PATH, output);
  }
  FirmwareInstallIntentResult persist(const FirmwareInstallAuthorization& request) {
    if (scratch.size() < FIRMWARE_INSTALL_INTENT_SIZE || !validFirmwareInstallAuthorization(request))
      return FirmwareInstallIntentResult::Invalid;
    FirmwareInstallAuthorization saved;
    auto result = load(saved);
    if (result == FirmwareInstallIntentResult::Ok) {
      if (saved != request) return FirmwareInstallIntentResult::Conflict;
      // A second staging record cannot be silently discarded.
      result = read(FIRMWARE_INSTALL_INTENT_STAGE, saved);
      return result == FirmwareInstallIntentResult::Missing ? FirmwareInstallIntentResult::Ok
             : result == FirmwareInstallIntentResult::Ok    ? FirmwareInstallIntentResult::Conflict
                                                            : result;
    }
    if (result != FirmwareInstallIntentResult::Missing) return result;
    result = read(FIRMWARE_INSTALL_INTENT_STAGE, saved);
    if (result == FirmwareInstallIntentResult::Ok) {
      if (saved != request) return FirmwareInstallIntentResult::Conflict;
    } else if (result == FirmwareInstallIntentResult::Missing) {
      auto bytes = scratch.first(FIRMWARE_INSTALL_INTENT_SIZE);
      if (!encodeFirmwareInstallAuthorization(request, bytes.first(FIRMWARE_INSTALL_AUTHORIZATION_SIZE)))
        return FirmwareInstallIntentResult::Invalid;
      const auto crc = courseBindingCrc(bytes.first(FIRMWARE_INSTALL_AUTHORIZATION_SIZE));
      for (size_t i = 0; i < 4; ++i)
        bytes[FIRMWARE_INSTALL_AUTHORIZATION_SIZE + i] = static_cast<uint8_t>(crc >> (8 * i));
      if (!storage.write(FIRMWARE_INSTALL_INTENT_STAGE, 0, bytes, true)) return FirmwareInstallIntentResult::IoError;
      result = read(FIRMWARE_INSTALL_INTENT_STAGE, saved);
      if (result != FirmwareInstallIntentResult::Ok) return result;
      if (saved != request) return FirmwareInstallIntentResult::Conflict;
    } else {
      return result;
    }
    uint64_t size = 0;
    const auto status = storage.stat(FIRMWARE_INSTALL_INTENT_PATH, size);
    if (status == FileStatus::Error) return FirmwareInstallIntentResult::IoError;
    if (status != FileStatus::Missing) return FirmwareInstallIntentResult::Conflict;
    if (!storage.rename(FIRMWARE_INSTALL_INTENT_STAGE, FIRMWARE_INSTALL_INTENT_PATH))
      return FirmwareInstallIntentResult::IoError;
    result = load(saved);
    return result == FirmwareInstallIntentResult::Ok && saved != request ? FirmwareInstallIntentResult::Conflict
                                                                         : result;
  }

  FirmwareInstallIntentResult retireVerified(const FirmwareReaderInfo& running) {
    if (!validFirmwareReaderInfo(running)) return FirmwareInstallIntentResult::Invalid;
    return retireVerifiedImage(running.generation, running.runningBuild);
  }
  // Exact complete-image equality includes the board/chip bytes validated at staging.
  FirmwareInstallIntentResult retireVerifiedImage(const Identity& generation, const Digest& runningHash) {
    FirmwareInstallAuthorization saved;
    auto result = load(saved);
    if (result != FirmwareInstallIntentResult::Ok) return result;
    if (saved.request.generation != generation || saved.request.hash != runningHash)
      return FirmwareInstallIntentResult::Conflict;
    uint64_t size = 0;
    const auto stage = storage.stat(FIRMWARE_INSTALL_INTENT_STAGE, size);
    if (stage == FileStatus::Error) return FirmwareInstallIntentResult::IoError;
    if (stage != FileStatus::Missing) return FirmwareInstallIntentResult::Conflict;
    if (!storage.remove(FIRMWARE_INSTALL_INTENT_PATH)) return FirmwareInstallIntentResult::IoError;
    const auto final = storage.stat(FIRMWARE_INSTALL_INTENT_PATH, size);
    if (final == FileStatus::Error) return FirmwareInstallIntentResult::IoError;
    return final == FileStatus::Missing ? FirmwareInstallIntentResult::Ok : FirmwareInstallIntentResult::Conflict;
  }

 private:
  FirmwareInstallIntentResult read(const char* path, FirmwareInstallAuthorization& output) {
    if (scratch.size() < FIRMWARE_INSTALL_INTENT_SIZE) return FirmwareInstallIntentResult::Invalid;
    uint64_t size = 0;
    const auto status = storage.stat(path, size);
    if (status == FileStatus::Missing) return FirmwareInstallIntentResult::Missing;
    if (status == FileStatus::Error) return FirmwareInstallIntentResult::IoError;
    if (size != FIRMWARE_INSTALL_INTENT_SIZE) return FirmwareInstallIntentResult::Corrupt;
    auto bytes = scratch.first(FIRMWARE_INSTALL_INTENT_SIZE);
    if (!storage.read(path, 0, bytes)) return FirmwareInstallIntentResult::IoError;
    uint32_t crc = 0;
    for (size_t i = 0; i < 4; ++i) crc |= uint32_t(bytes[FIRMWARE_INSTALL_AUTHORIZATION_SIZE + i]) << (8 * i);
    if (crc != courseBindingCrc(bytes.first(FIRMWARE_INSTALL_AUTHORIZATION_SIZE)) ||
        !decodeFirmwareInstallAuthorization(bytes.first(FIRMWARE_INSTALL_AUTHORIZATION_SIZE), output))
      return FirmwareInstallIntentResult::Corrupt;
    return FirmwareInstallIntentResult::Ok;
  }
  TransferStorage& storage;
  std::span<uint8_t> scratch;
};
}  // namespace companion

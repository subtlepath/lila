#pragma once

#include "CompanionFirmwareReaderInfo.h"
#include "CompanionFirmwareTransfer.h"
#include "CompanionTransfer.h"

namespace companion {
inline constexpr size_t FIRMWARE_INSTALL_REQUEST_SIZE = 104;
struct FirmwareInstallRequest {
  Identity generation{}, transaction{};
  Digest hash{};
  uint64_t length = 0, partitionBytes = 0;
  uint32_t minimumSchema = 0, maximumSchema = 0, journalVersions = 0;
  uint16_t chip = 0xffff;
  uint8_t minimumProtocol = 1, maximumProtocol = 1, minimumBattery = 30;
  Board board = Board::X4;
  bool operator==(const FirmwareInstallRequest&) const = default;
};
inline bool validFirmwareInstallRequest(const FirmwareInstallRequest& request) {
  const auto nonzero = [](const auto& bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte != 0; });
  };
  const auto board = static_cast<uint8_t>(request.board);
  return nonzero(request.generation) && nonzero(request.transaction) && nonzero(request.hash) &&
         request.length >= 65536 && request.length <= request.partitionBytes && request.partitionBytes <= UINT32_MAX &&
         request.minimumSchema <= request.maximumSchema && (request.journalVersions & ~uint32_t{7}) == 0 &&
         request.minimumProtocol > 0 && request.minimumProtocol <= 1 && request.maximumProtocol >= 1 &&
         request.minimumBattery >= 30 && request.minimumBattery <= 100 && board >= 1 && board <= 5 &&
         request.chip == (board == 1 ? 5 : 9);
}
inline bool encodeFirmwareInstallRequest(const FirmwareInstallRequest& request, std::span<uint8_t> output) {
  if (output.size() != FIRMWARE_INSTALL_REQUEST_SIZE || !validFirmwareInstallRequest(request)) return false;
  std::fill(output.begin(), output.end(), 0);
  output[0] = 'F';
  output[1] = 'W';
  output[2] = 'F';
  output[3] = 1;
  std::copy(request.generation.begin(), request.generation.end(), output.begin() + 4);
  std::copy(request.transaction.begin(), request.transaction.end(), output.begin() + 20);
  std::copy(request.hash.begin(), request.hash.end(), output.begin() + 36);
  for (size_t i = 0; i < 8; ++i) {
    output[68 + i] = static_cast<uint8_t>(request.length >> (8 * i));
    output[96 + i] = static_cast<uint8_t>(request.partitionBytes >> (8 * i));
  }
  for (size_t i = 0; i < 4; ++i) {
    output[76 + i] = static_cast<uint8_t>(request.minimumSchema >> (8 * i));
    output[80 + i] = static_cast<uint8_t>(request.maximumSchema >> (8 * i));
    output[84 + i] = static_cast<uint8_t>(request.journalVersions >> (8 * i));
  }
  output[88] = request.minimumProtocol;
  output[89] = request.maximumProtocol;
  output[90] = request.minimumBattery;
  output[91] = static_cast<uint8_t>(request.board);
  output[92] = static_cast<uint8_t>(request.chip);
  output[93] = static_cast<uint8_t>(request.chip >> 8);
  return true;
}
inline bool decodeFirmwareInstallRequest(std::span<const uint8_t> bytes, FirmwareInstallRequest& output) {
  if (bytes.size() != FIRMWARE_INSTALL_REQUEST_SIZE || bytes[0] != 'F' || bytes[1] != 'W' || bytes[2] != 'F' ||
      bytes[3] != 1 || bytes[94] || bytes[95])
    return false;
  FirmwareInstallRequest request;
  std::copy_n(bytes.begin() + 4, 16, request.generation.begin());
  std::copy_n(bytes.begin() + 20, 16, request.transaction.begin());
  std::copy_n(bytes.begin() + 36, 32, request.hash.begin());
  for (size_t i = 0; i < 8; ++i) {
    request.length |= uint64_t(bytes[68 + i]) << (8 * i);
    request.partitionBytes |= uint64_t(bytes[96 + i]) << (8 * i);
  }
  for (size_t i = 0; i < 4; ++i) {
    request.minimumSchema |= uint32_t(bytes[76 + i]) << (8 * i);
    request.maximumSchema |= uint32_t(bytes[80 + i]) << (8 * i);
    request.journalVersions |= uint32_t(bytes[84 + i]) << (8 * i);
  }
  request.minimumProtocol = bytes[88];
  request.maximumProtocol = bytes[89];
  request.minimumBattery = bytes[90];
  request.board = static_cast<Board>(bytes[91]);
  request.chip = uint16_t(bytes[92]) | (uint16_t(bytes[93]) << 8);
  if (!validFirmwareInstallRequest(request)) return false;
  output = request;
  return true;
}
inline bool firmwareInstallCompatible(const FirmwareInstallRequest& request, const FirmwareReaderInfo& info) {
  return validFirmwareInstallRequest(request) && validFirmwareReaderInfo(info) &&
         request.generation == info.generation && request.board == info.board && request.chip == info.chip &&
         request.partitionBytes == info.partitionBytes && info.battery >= request.minimumBattery &&
         info.minimumProtocol <= 1 && info.maximumProtocol >= 1 && info.stateSchema >= request.minimumSchema &&
         info.stateSchema <= request.maximumSchema && (info.journalVersions & ~request.journalVersions) == 0;
}
// The authenticated caller supplies owner; current SD bytes must still be hashed before flashing.
inline bool firmwareInstallMatchesTransfer(const FirmwareInstallRequest& request, const Identity& owner,
                                           const TransferState& state, const ContentManifest& manifest,
                                           std::string_view destination) {
  return validFirmwareInstallRequest(request) && validFirmwareStageManifest(manifest) &&
         destination == FIRMWARE_STAGE_DESTINATION && state.phase == TransferPhase::Committed &&
         state.transaction == request.transaction && state.owner == owner &&
         state.storageGeneration == request.generation && state.contentHash == request.hash &&
         state.length == request.length && state.durableOffset == state.length &&
         manifest.contentHash == request.hash && manifest.length == request.length;
}
}  // namespace companion

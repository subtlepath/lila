#pragma once

#include <algorithm>

#include "CompanionRecords.h"

namespace companion {
inline constexpr size_t FIRMWARE_INFO_REQUEST_SIZE = 20;
inline constexpr size_t FIRMWARE_READER_INFO_SIZE = 76;
struct FirmwareReaderInfo {
  Board board = Board::X4;
  uint8_t battery = 0, minimumProtocol = 1, maximumProtocol = 1;
  Identity generation{};
  Digest runningBuild{};
  uint16_t chip = 0xffff;
  uint64_t partitionBytes = 0;
  uint32_t stateSchema = 0, journalVersions = 0;
  bool operator==(const FirmwareReaderInfo&) const = default;
};
inline bool validFirmwareReaderInfo(const FirmwareReaderInfo& info) {
  const auto nonzero = [](const auto& bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte != 0; });
  };
  return static_cast<unsigned>(info.board) >= 1 && static_cast<unsigned>(info.board) <= 5 && info.battery <= 100 &&
         info.minimumProtocol > 0 && info.minimumProtocol <= info.maximumProtocol && nonzero(info.generation) &&
         nonzero(info.runningBuild) && info.chip != 0xffff && info.partitionBytes >= 65536 &&
         info.partitionBytes <= UINT32_MAX && (info.journalVersions & ~uint32_t{7}) == 0;
}
inline bool encodeFirmwareReaderInfo(const FirmwareReaderInfo& info, std::span<uint8_t> output) {
  if (output.size() != FIRMWARE_READER_INFO_SIZE || !validFirmwareReaderInfo(info)) return false;
  std::fill(output.begin(), output.end(), 0);
  output[0] = 'F';
  output[1] = 'W';
  output[2] = 'I';
  output[3] = 1;
  output[4] = static_cast<uint8_t>(info.board);
  output[5] = info.battery;
  output[6] = info.minimumProtocol;
  output[7] = info.maximumProtocol;
  std::copy(info.generation.begin(), info.generation.end(), output.begin() + 8);
  std::copy(info.runningBuild.begin(), info.runningBuild.end(), output.begin() + 24);
  for (size_t i = 0; i < 2; ++i) output[56 + i] = static_cast<uint8_t>(info.chip >> (8 * i));
  for (size_t i = 0; i < 8; ++i) output[60 + i] = static_cast<uint8_t>(info.partitionBytes >> (8 * i));
  for (size_t i = 0; i < 4; ++i) {
    output[68 + i] = static_cast<uint8_t>(info.stateSchema >> (8 * i));
    output[72 + i] = static_cast<uint8_t>(info.journalVersions >> (8 * i));
  }
  return true;
}
inline bool decodeFirmwareReaderInfo(std::span<const uint8_t> bytes, FirmwareReaderInfo& output) {
  if (bytes.size() != FIRMWARE_READER_INFO_SIZE || bytes[0] != 'F' || bytes[1] != 'W' || bytes[2] != 'I' ||
      bytes[3] != 1 || bytes[58] || bytes[59])
    return false;
  FirmwareReaderInfo info;
  info.board = static_cast<Board>(bytes[4]);
  info.battery = bytes[5];
  info.minimumProtocol = bytes[6];
  info.maximumProtocol = bytes[7];
  std::copy_n(bytes.begin() + 8, 16, info.generation.begin());
  std::copy_n(bytes.begin() + 24, 32, info.runningBuild.begin());
  info.chip = uint16_t(bytes[56]) | (uint16_t(bytes[57]) << 8);
  for (size_t i = 0; i < 8; ++i) info.partitionBytes |= uint64_t(bytes[60 + i]) << (8 * i);
  for (size_t i = 0; i < 4; ++i) {
    info.stateSchema |= uint32_t(bytes[68 + i]) << (8 * i);
    info.journalVersions |= uint32_t(bytes[72 + i]) << (8 * i);
  }
  if (!validFirmwareReaderInfo(info)) return false;
  output = info;
  return true;
}
}  // namespace companion

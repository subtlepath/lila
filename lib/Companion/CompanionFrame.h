#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace companion {

inline constexpr uint8_t PROTOCOL_VERSION = 1;
inline constexpr size_t FRAME_HEADER_SIZE = 12;
inline constexpr size_t MAX_CONTROL_PAYLOAD = 1024;
inline constexpr size_t JOURNAL_EXPORT_REQUEST_SIZE = 40;
inline constexpr size_t SESSION_WORKSPACE_SIZE = 8192;
inline constexpr size_t MAX_INVENTORY_ITEMS = 8;
inline constexpr size_t MAX_PENDING_COMMANDS = 4;
inline constexpr uint64_t WIFI_TRANSFER_THRESHOLD = 1024 * 1024;

enum class Command : uint8_t {
  Discover = 1,
  Inventory = 2,
  ExchangeChanges = 3,
  BeginTransfer = 4,
  TransferChunk = 5,
  TransferStatus = 6,
  Commit = 7,
  Abort = 8,
  WifiHandoff = 9,
  InstallFirmware = 10,
  Error = 11,
  RegisterInstallation = 12,
  AuthenticateInstallation = 13,
  JournalFormats = 14,
  RemoveContent = 15,
};

enum class FrameError : uint8_t { None, Truncated, Magic, Version, Command, Flags, Length, Unauthorized };

struct FrameView {
  Command command = Command::Discover;
  bool response = false;
  uint32_t requestId = 0;
  std::span<const uint8_t> payload;
};

inline bool validCommand(uint8_t command) {
  return command >= static_cast<uint8_t>(Command::Discover) && command <= static_cast<uint8_t>(Command::RemoveContent);
}

// Authentication is supplied by the bonded BLE or authenticated HTTP transport.
// Discovery is the only command accepted before transport authentication.
inline FrameError decodeFrame(std::span<const uint8_t> bytes, bool authenticated, FrameView& output) {
  if (bytes.size() < FRAME_HEADER_SIZE) return FrameError::Truncated;
  if (bytes[0] != 'L' || bytes[1] != 'C') return FrameError::Magic;
  if (bytes[2] != PROTOCOL_VERSION) return FrameError::Version;
  if (!validCommand(bytes[3])) return FrameError::Command;
  if ((bytes[4] & ~1U) != 0 || bytes[5] != 0) return FrameError::Flags;
  const size_t length = static_cast<size_t>(bytes[10]) | (static_cast<size_t>(bytes[11]) << 8U);
  if (length > MAX_CONTROL_PAYLOAD || bytes.size() != FRAME_HEADER_SIZE + length) return FrameError::Length;
  if (!authenticated && bytes[3] != static_cast<uint8_t>(Command::Discover)) return FrameError::Unauthorized;
  FrameView parsed;
  parsed.command = static_cast<Command>(bytes[3]);
  parsed.response = (bytes[4] & 1U) != 0;
  for (unsigned i = 0; i < 4; ++i) parsed.requestId |= static_cast<uint32_t>(bytes[6 + i]) << (8U * i);
  parsed.payload = bytes.subspan(FRAME_HEADER_SIZE, length);
  output = parsed;
  return FrameError::None;
}

// Payload and destination must not overlap. Returns zero without modifying the
// destination when validation fails. The caller owns all storage.
inline size_t encodeFrame(const FrameView& frame, std::span<uint8_t> destination) {
  if (!validCommand(static_cast<uint8_t>(frame.command)) || frame.payload.size() > MAX_CONTROL_PAYLOAD ||
      destination.size() < FRAME_HEADER_SIZE + frame.payload.size()) {
    return 0;
  }
  destination[0] = 'L';
  destination[1] = 'C';
  destination[2] = PROTOCOL_VERSION;
  destination[3] = static_cast<uint8_t>(frame.command);
  destination[4] = frame.response ? 1 : 0;
  destination[5] = 0;
  for (unsigned i = 0; i < 4; ++i) destination[6 + i] = static_cast<uint8_t>(frame.requestId >> (8U * i));
  destination[10] = static_cast<uint8_t>(frame.payload.size());
  destination[11] = static_cast<uint8_t>(frame.payload.size() >> 8U);
  for (size_t i = 0; i < frame.payload.size(); ++i) destination[FRAME_HEADER_SIZE + i] = frame.payload[i];
  return FRAME_HEADER_SIZE + frame.payload.size();
}

}  // namespace companion

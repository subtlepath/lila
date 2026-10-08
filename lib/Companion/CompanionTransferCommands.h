#pragma once

#include <algorithm>
#include <span>
#include <string_view>

#include "CompanionFrame.h"
#include "CompanionTransfer.h"

namespace companion {
// Views borrow the command payload and expire when its input buffer is reused.
struct BeginTransferCommand {
  TransferState state;
  std::string_view destination;
};
struct TransferChunkCommand {
  Identity transaction;
  uint64_t offset = 0;
  std::span<const uint8_t> bytes;
};
inline constexpr size_t CHUNK_HEADER_SIZE = 24;
inline constexpr size_t MAX_CHUNK_SIZE = MAX_CONTROL_PAYLOAD - CHUNK_HEADER_SIZE;

inline bool decodeBeginTransfer(std::span<const uint8_t> payload, BeginTransferCommand& output) {
  if (payload.size() <= TRANSFER_STATE_SIZE) return false;
  const size_t length = payload[TRANSFER_STATE_SIZE];
  if (!length || length >= TRANSFER_TARGET_SIZE || payload.size() != TRANSFER_STATE_SIZE + 1 + length) return false;
  const auto path = payload.subspan(TRANSFER_STATE_SIZE + 1);
  if (path[0] != '/' || std::find(path.begin(), path.end(), uint8_t{0}) != path.end()) return false;
  TransferState state;
  if (!decodeRecord(payload.first(TRANSFER_STATE_SIZE), state) || state.phase != TransferPhase::Receiving ||
      state.durableOffset != 0)
    return false;
  output.state = state;
  output.destination = {reinterpret_cast<const char*>(path.data()), path.size()};
  return true;
}

inline bool decodeTransferChunk(std::span<const uint8_t> payload, TransferChunkCommand& output) {
  if (payload.size() <= CHUNK_HEADER_SIZE || payload.size() > MAX_CONTROL_PAYLOAD) return false;
  TransferChunkCommand next;
  std::copy_n(payload.begin(), next.transaction.size(), next.transaction.begin());
  for (size_t i = 0; i < 8; ++i) next.offset |= static_cast<uint64_t>(payload[16 + i]) << (8 * i);
  next.bytes = payload.subspan(CHUNK_HEADER_SIZE);
  output = next;
  return true;
}

inline bool decodeTransaction(std::span<const uint8_t> payload, Identity& output) {
  if (payload.size() != output.size()) return false;
  std::copy(payload.begin(), payload.end(), output.begin());
  return true;
}
}  // namespace companion

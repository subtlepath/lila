#pragma once

#include <algorithm>
#include <string_view>

#include "CompanionTransferDeclaration.h"

namespace companion {
// Destination borrows the command payload and expires with its input buffer.
struct DeclaredBeginTransferCommand {
  TransferDeclaration declaration;
  std::string_view destination;
};
static_assert(sizeof(DeclaredBeginTransferCommand) < 256);
inline constexpr size_t DECLARED_TRANSFER_TARGET_SIZE = 128;
// Keep declaration decode scratch separate from the caller's parsed command.
[[gnu::noinline]] inline bool decodeDeclaredBeginTransfer(std::span<const uint8_t> payload,
                                                          DeclaredBeginTransferCommand& output) {
  if (payload.size() <= TRANSFER_DECLARATION_SIZE) return false;
  const size_t length = payload[TRANSFER_DECLARATION_SIZE];
  if (length < 2 || length >= DECLARED_TRANSFER_TARGET_SIZE || payload.size() != TRANSFER_DECLARATION_SIZE + 1 + length)
    return false;
  const auto path = payload.subspan(TRANSFER_DECLARATION_SIZE + 1);
  if (path.front() != '/' ||
      std::any_of(path.begin(), path.end(), [](uint8_t byte) { return byte < 32 || byte == 127; }))
    return false;
  if (!decodeTransferDeclaration(payload.first(TRANSFER_DECLARATION_SIZE), output.declaration)) return false;
  output.destination = {reinterpret_cast<const char*>(path.data()), path.size()};
  return true;
}
}  // namespace companion

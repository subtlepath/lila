#pragma once
#include "CompanionTransferCommands.h"
namespace companion {
inline constexpr char EPUB_DIRECTORY[] = "/Books/Companion";
// Result byte followed by TransferState on success. owner comes exclusively
// from the authenticated installation session, never from an untrusted body.
size_t handleTransfer(Transfer& transfer, Command command, std::span<const uint8_t> body, const Identity& owner,
                      std::span<uint8_t> response);
struct TransferDispatchOutcome {
  size_t length = 0;
  bool inventoryChanged = false;
  bool recoveryBlocked = false;
};
TransferDispatchOutcome dispatchTransfer(Transfer& transfer, const Identity& generation, Command command,
                                         std::span<const uint8_t> body, const Identity& owner,
                                         std::span<uint8_t> response);
}  // namespace companion

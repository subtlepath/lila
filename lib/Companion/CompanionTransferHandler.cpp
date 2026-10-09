#include "CompanionTransferHandler.h"

#include "../EpdFont/VectorFontSupport.h"
#include "CompanionCourseBinding.h"
#include "CompanionDeclaredTransferCommand.h"
#include "CompanionDictionaryArchiveBinding.h"
#include "CompanionFirmwareTransfer.h"
#include "CompanionFontDestination.h"
namespace companion {
namespace {
bool validHashedTarget(std::string_view destination, const Digest& hash, std::string_view prefix,
                       std::string_view suffix) {
  if (!destination.starts_with(prefix) || !destination.ends_with(suffix) ||
      destination.size() != prefix.size() + 64 + suffix.size())
    return false;
  static constexpr char HEX[] = "0123456789abcdef";
  for (size_t i = 0; i < hash.size(); ++i) {
    if (destination[prefix.size() + i * 2] != HEX[hash[i] >> 4] ||
        destination[prefix.size() + i * 2 + 1] != HEX[hash[i] & 15])
      return false;
  }
  return true;
}
[[gnu::noinline]] TransferResult finishDeclaredBegin(Transfer& transfer, const DeclaredBeginTransferCommand& begin,
                                                     const Identity& owner) {
  const auto& manifest = begin.declaration.manifest;
  if (manifest.kind == ContentKind::Epub) {
    if (!validHashedTarget(begin.destination, manifest.contentHash, "/Books/Companion/", ".epub"))
      return TransferResult::Invalid;
  } else if (manifest.kind == ContentKind::Font) {
    if (!validFontDestination(begin.destination, manifest.formatVersion)) return TransferResult::Invalid;
#if !CROSSPOINT_VECTOR_FONTS
    if (manifest.formatVersion == 1) return TransferResult::Invalid;
#endif
  } else if (manifest.kind == ContentKind::Dictionary) {
    if (!validDictionaryBindingManifest(manifest) ||
        !validHashedTarget(begin.destination, manifest.contentHash, "/dictionaries/", "/dictionary"))
      return TransferResult::Invalid;
  } else if (manifest.kind == ContentKind::Firmware) {
    if (!validFirmwareStageManifest(manifest) || begin.destination != FIRMWARE_STAGE_DESTINATION)
      return TransferResult::Invalid;
  } else {
#if LILA_TINTA
    if (manifest.kind != ContentKind::Course || manifest.formatVersion != 1 || begin.destination != ACTIVE_COURSE_PATH)
      return TransferResult::Invalid;
#else
    return TransferResult::Invalid;
#endif
  }
  if (begin.declaration.state.owner != owner) return TransferResult::Unauthorized;
  return transfer.begin(begin.declaration, begin.destination);
}
// Keep the parsed declaration outside the generic dispatch and validation frames.
[[gnu::noinline]] TransferResult declaredBegin(Transfer& transfer, std::span<const uint8_t> body,
                                               const Identity& owner) {
  DeclaredBeginTransferCommand begin;
  if (!decodeDeclaredBeginTransfer(body, begin)) return TransferResult::Invalid;
  return finishDeclaredBegin(transfer, begin, owner);
}
TransferResult execute(Transfer& transfer, Command command, std::span<const uint8_t> body, const Identity& owner) {
  if (std::all_of(owner.begin(), owner.end(), [](uint8_t value) { return value == 0; }))
    return TransferResult::Unauthorized;
  if (command == Command::BeginTransfer) {
    if (body.size() >= 2 && body[1] == static_cast<uint8_t>(RecordKind::ContentManifest))
      return declaredBegin(transfer, body, owner);
    BeginTransferCommand begin;
    if (!decodeBeginTransfer(body, begin) ||
        !validHashedTarget(begin.destination, begin.state.contentHash, "/Books/Companion/", ".epub"))
      return TransferResult::Invalid;
    if (begin.state.owner != owner) return TransferResult::Unauthorized;
    return transfer.begin(begin.state, begin.destination);
  }
  if (command == Command::TransferChunk) {
    TransferChunkCommand chunk;
    if (!decodeTransferChunk(body, chunk)) return TransferResult::Invalid;
    return transfer.append(chunk.transaction, owner, chunk.offset, chunk.bytes);
  }
  Identity transaction;
  if (!decodeTransaction(body, transaction)) return TransferResult::Invalid;
  if (command == Command::Commit) return transfer.commit(transaction, owner);
  if (command == Command::Abort) return transfer.abort(transaction, owner);
  if (command != Command::TransferStatus) return TransferResult::Invalid;
  const auto* current = transfer.current();
  if (!current) return TransferResult::NoTransaction;
  if (current->owner != owner) return TransferResult::Unauthorized;
  return current->transaction == transaction ? TransferResult::Ok : TransferResult::Invalid;
}
}  // namespace
size_t handleTransfer(Transfer& transfer, Command command, std::span<const uint8_t> body, const Identity& owner,
                      std::span<uint8_t> response) {
  if (response.size() < 1 + TRANSFER_STATE_SIZE) return 0;
  const auto result = execute(transfer, command, body, owner);
  response[0] = static_cast<uint8_t>(result);
  if (result != TransferResult::Ok) return 1;
  const auto* current = transfer.current();
  if (!current) return 1;
  return 1 + encodeRecord(*current, response.subspan(1));
}
TransferDispatchOutcome dispatchTransfer(Transfer& transfer, const Identity& generation, Command command,
                                         std::span<const uint8_t> body, const Identity& owner,
                                         std::span<uint8_t> response) {
  const bool wasCommitted = transfer.current() && transfer.current()->phase == TransferPhase::Committed;
  TransferDispatchOutcome outcome;
  outcome.length = handleTransfer(transfer, command, body, owner, response);
  if (!outcome.length) return outcome;
  if (response[0] == static_cast<uint8_t>(TransferResult::IoError))
    outcome.recoveryBlocked = transfer.recover(generation) != TransferResult::Ok;
  const auto* state = transfer.current();
  outcome.inventoryChanged = !wasCommitted && state && state->phase == TransferPhase::Committed;
  if (state && state->phase == TransferPhase::Installing && response[0] != 0) outcome.recoveryBlocked = true;
  return outcome;
}
}  // namespace companion

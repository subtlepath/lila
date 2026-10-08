#pragma once

#include "CompanionRecords.h"

namespace companion {

inline constexpr size_t TRANSFER_DECLARATION_SIZE = CONTENT_MANIFEST_SIZE + TRANSFER_STATE_SIZE;

struct TransferDeclaration {
  ContentManifest manifest{};
  TransferState state{};
  bool operator==(const TransferDeclaration&) const = default;
};
static_assert(sizeof(TransferDeclaration) < 256);

inline bool matchesTransferManifest(const ContentManifest& manifest, const TransferState& state) {
  if (manifest.length == 0 || manifest.length != state.length || manifest.contentHash != state.contentHash)
    return false;
  const auto kind = static_cast<uint8_t>(manifest.kind);
  if (kind < 1 || kind > 5) return false;
  bool hasIdentity = false;
  for (const auto byte : manifest.logicalIdentity) hasIdentity |= byte != 0;
  return manifest.kind == ContentKind::Course ? manifest.formatVersion > 0 && hasIdentity : !hasIdentity;
}

inline bool validTransferDeclaration(const TransferDeclaration& declaration) {
  return declaration.state.phase == TransferPhase::Receiving && declaration.state.durableOffset == 0 &&
         matchesTransferManifest(declaration.manifest, declaration.state);
}

inline size_t encodeTransferDeclaration(const TransferDeclaration& declaration, std::span<uint8_t> output) {
  if (output.size() < TRANSFER_DECLARATION_SIZE || !validTransferDeclaration(declaration)) return 0;
  if (encodeRecord(declaration.manifest, output.first(CONTENT_MANIFEST_SIZE)) != CONTENT_MANIFEST_SIZE) return 0;
  if (encodeRecord(declaration.state, output.subspan(CONTENT_MANIFEST_SIZE, TRANSFER_STATE_SIZE)) !=
      TRANSFER_STATE_SIZE)
    return 0;
  return TRANSFER_DECLARATION_SIZE;
}

inline bool decodeTransferDeclaration(std::span<const uint8_t> input, TransferDeclaration& declaration) {
  if (input.size() != TRANSFER_DECLARATION_SIZE) return false;
  TransferDeclaration parsed;
  if (!decodeRecord(input.first(CONTENT_MANIFEST_SIZE), parsed.manifest) ||
      !decodeRecord(input.subspan(CONTENT_MANIFEST_SIZE), parsed.state) || !validTransferDeclaration(parsed)) {
    return false;
  }
  declaration = parsed;
  return true;
}

}  // namespace companion

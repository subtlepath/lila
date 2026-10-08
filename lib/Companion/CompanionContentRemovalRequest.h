#pragma once

#include <algorithm>

#include "CompanionRecords.h"

namespace companion {
inline constexpr size_t CONTENT_REMOVAL_REQUEST_SIZE = 115;
struct ContentRemovalRequest {
  Identity transaction{}, owner{}, generation{};
  ContentManifest manifest{};
  bool operator==(const ContentRemovalRequest&) const = default;
};
static_assert(sizeof(ContentRemovalRequest) < 256);
inline bool validContentRemovalRequest(const ContentRemovalRequest& request) {
  const auto nonzero = [](const auto& bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte != 0; });
  };
  if (!nonzero(request.transaction) || !nonzero(request.owner) || !nonzero(request.generation) ||
      !nonzero(request.manifest.contentHash) || request.manifest.length == 0)
    return false;
  const auto& manifest = request.manifest;
  const bool family = nonzero(manifest.logicalIdentity);
  switch (manifest.kind) {
    case ContentKind::Epub:
      return manifest.formatVersion <= 1 && !family;
    case ContentKind::Course:
      return manifest.formatVersion == 1 && family;
    case ContentKind::Font:
      return (manifest.formatVersion == 1 || manifest.formatVersion == 2) && !family;
    case ContentKind::Dictionary:
      return manifest.formatVersion == 1 && !family;
    default:
      return false;
  }
}
inline size_t encodeContentRemovalRequest(const ContentRemovalRequest& request, std::span<uint8_t> output) {
  if (output.size() < CONTENT_REMOVAL_REQUEST_SIZE || !validContentRemovalRequest(request)) return 0;
  output[0] = 'L';
  output[1] = 'R';
  output[2] = 'M';
  output[3] = 1;
  std::copy(request.transaction.begin(), request.transaction.end(), output.begin() + 4);
  std::copy(request.owner.begin(), request.owner.end(), output.begin() + 20);
  std::copy(request.generation.begin(), request.generation.end(), output.begin() + 36);
  return encodeRecord(request.manifest, output.subspan(52, CONTENT_MANIFEST_SIZE)) == CONTENT_MANIFEST_SIZE
             ? CONTENT_REMOVAL_REQUEST_SIZE
             : 0;
}
inline bool decodeContentRemovalRequest(std::span<const uint8_t> input, ContentRemovalRequest& output) {
  if (input.size() != CONTENT_REMOVAL_REQUEST_SIZE || input[0] != 'L' || input[1] != 'R' || input[2] != 'M' ||
      input[3] != 1)
    return false;
  ContentRemovalRequest parsed;
  std::copy_n(input.begin() + 4, 16, parsed.transaction.begin());
  std::copy_n(input.begin() + 20, 16, parsed.owner.begin());
  std::copy_n(input.begin() + 36, 16, parsed.generation.begin());
  if (!decodeRecord(input.subspan(52), parsed.manifest) || !validContentRemovalRequest(parsed)) return false;
  output = parsed;
  return true;
}
}  // namespace companion

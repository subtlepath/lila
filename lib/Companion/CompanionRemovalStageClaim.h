#pragma once

#include "CompanionMultiPathRemovalPlan.h"

namespace companion {
inline constexpr size_t REMOVAL_STAGE_CLAIM_SIZE = 135;
struct RemovalStageClaim {
  ContentRemovalRequest request;
  uint64_t inventoryRevision = 0;
  bool operator==(const RemovalStageClaim&) const = default;
};
inline bool validRemovalStageClaim(const RemovalStageClaim& claim) {
  return validContentRemovalRequest(claim.request) && claim.inventoryRevision != 0 &&
         (claim.request.manifest.kind == ContentKind::Epub || claim.request.manifest.kind == ContentKind::Font ||
          claim.request.manifest.kind == ContentKind::Dictionary || claim.request.manifest.kind == ContentKind::Course);
}
inline size_t encodeRemovalStageClaim(const RemovalStageClaim& claim, std::span<uint8_t> output) {
  if (output.size() < REMOVAL_STAGE_CLAIM_SIZE || !validRemovalStageClaim(claim)) return 0;
  static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'R', 'S', 'C', 1, 0, 0, 0};
  std::copy(PREFIX.begin(), PREFIX.end(), output.begin());
  if (!encodeContentRemovalRequest(claim.request, output.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE))) return 0;
  inventory_detail::write(output, 123, claim.inventoryRevision, 8);
  inventory_detail::write(output, 131, inventoryIndexCrc(output.first(131)), 4);
  return REMOVAL_STAGE_CLAIM_SIZE;
}
inline bool decodeRemovalStageClaim(std::span<const uint8_t> bytes, RemovalStageClaim& output) {
  static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'R', 'S', 'C', 1, 0, 0, 0};
  if (bytes.size() != REMOVAL_STAGE_CLAIM_SIZE || !std::equal(PREFIX.begin(), PREFIX.end(), bytes.begin()) ||
      inventory_detail::read(bytes, 131, 4) != inventoryIndexCrc(bytes.first(131)))
    return false;
  RemovalStageClaim parsed;
  if (!multi_path_removal_detail::decodeRequest(bytes.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE), parsed.request))
    return false;
  parsed.inventoryRevision = inventory_detail::read(bytes, 123, 8);
  if (!validRemovalStageClaim(parsed)) return false;
  output = parsed;
  return true;
}
static_assert(sizeof(RemovalStageClaim) < 256);
}  // namespace companion

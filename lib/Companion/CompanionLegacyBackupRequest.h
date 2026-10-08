#pragma once

#include "CompanionLegacyTintaBackupManifest.h"

namespace companion {
enum class LegacyBackupOperation : uint8_t { Capture = 1, Manifest, File, Close };
inline constexpr size_t LEGACY_BACKUP_REQUEST_SIZE = 64;
inline constexpr size_t LEGACY_BACKUP_CHUNK_LIMIT = 768;
struct LegacyBackupRequest {
  LegacyBackupOperation operation = LegacyBackupOperation::Manifest;
  uint8_t role = 0xff;
  bool bound = false;
  Identity course{}, transaction{}, generation{};
  uint32_t offset = 0;
  uint16_t count = 0;
};
inline bool validLegacyBackupRequest(const LegacyBackupRequest& request) {
  const auto operation = static_cast<unsigned>(request.operation);
  if (operation < 1 || operation > 4 || !tinta_body_detail::nonzero(request.course) ||
      !tinta_body_detail::nonzero(request.transaction) || !tinta_body_detail::nonzero(request.generation))
    return false;
  if (request.operation == LegacyBackupOperation::File)
    return request.role < LEGACY_TINTA_BACKUP_ROLES && request.count > 0 && request.count <= LEGACY_BACKUP_CHUNK_LIMIT;
  return request.role == 0xff && request.offset == 0 && request.count == 0;
}
inline bool encodeLegacyBackupRequest(const LegacyBackupRequest& request, std::span<uint8_t> output) {
  if (output.size() != LEGACY_BACKUP_REQUEST_SIZE || !validLegacyBackupRequest(request)) return false;
  std::fill(output.begin(), output.end(), 0);
  output[0] = 'T';
  output[1] = 'L';
  output[2] = 'R';
  output[3] = 1;
  output[4] = static_cast<uint8_t>(request.operation);
  output[5] = request.role;
  output[6] = request.bound;
  std::copy(request.course.begin(), request.course.end(), output.begin() + 8);
  std::copy(request.transaction.begin(), request.transaction.end(), output.begin() + 24);
  std::copy(request.generation.begin(), request.generation.end(), output.begin() + 40);
  tinta_body_detail::write(output, 56, request.offset, 4);
  tinta_body_detail::write(output, 60, request.count, 2);
  return true;
}
inline bool decodeLegacyBackupRequest(std::span<const uint8_t> input, LegacyBackupRequest& output) {
  if (input.size() != LEGACY_BACKUP_REQUEST_SIZE || input[0] != 'T' || input[1] != 'L' || input[2] != 'R' ||
      input[3] != 1 || input[6] > 1 || input[7] || input[62] || input[63])
    return false;
  LegacyBackupRequest request;
  request.operation = static_cast<LegacyBackupOperation>(input[4]);
  request.role = input[5];
  request.bound = input[6];
  std::copy_n(input.begin() + 8, 16, request.course.begin());
  std::copy_n(input.begin() + 24, 16, request.transaction.begin());
  std::copy_n(input.begin() + 40, 16, request.generation.begin());
  request.offset = tinta_body_detail::read(input, 56, 4);
  request.count = tinta_body_detail::read(input, 60, 2);
  if (!validLegacyBackupRequest(request)) return false;
  output = request;
  return true;
}
}  // namespace companion

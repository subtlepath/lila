#pragma once

#include "CompanionLegacyBackupRequest.h"

namespace companion {
enum class LegacyBackupResult : uint8_t { Ok, Invalid, Conflict, IoError, Missing, Busy };
inline constexpr size_t LEGACY_BACKUP_REPLY_HEADER_SIZE = 32;
struct LegacyBackupReply {
  LegacyBackupResult result = LegacyBackupResult::Invalid;
  uint8_t role = 0xff;
  bool final = false;
  Identity transaction{};
  uint32_t offset = 0;
  std::span<const uint8_t> body;
};
inline bool validLegacyBackupReply(const LegacyBackupReply& reply) {
  if (static_cast<unsigned>(reply.result) > 5 || !tinta_body_detail::nonzero(reply.transaction) ||
      (reply.role != 0xff && reply.role >= LEGACY_TINTA_BACKUP_ROLES) ||
      reply.body.size() > LEGACY_BACKUP_CHUNK_LIMIT || reply.body.size() > UINT32_MAX - reply.offset)
    return false;
  if (reply.result != LegacyBackupResult::Ok) return reply.body.empty() && !reply.final;
  if (reply.role == 0xff)
    return reply.offset == 0 && reply.final &&
           (reply.body.empty() || reply.body.size() == LEGACY_TINTA_BACKUP_MANIFEST_SIZE);
  return !reply.body.empty() || reply.final;
}
// Body may already occupy the output tail; other overlaps are prohibited.
inline bool encodeLegacyBackupReply(const LegacyBackupReply& reply, std::span<uint8_t> output) {
  if (!validLegacyBackupReply(reply) || output.size() != LEGACY_BACKUP_REPLY_HEADER_SIZE + reply.body.size())
    return false;
  std::fill_n(output.begin(), LEGACY_BACKUP_REPLY_HEADER_SIZE, 0);
  output[0] = 'T';
  output[1] = 'L';
  output[2] = 'S';
  output[3] = 1;
  output[4] = static_cast<uint8_t>(reply.result);
  output[5] = reply.role;
  output[6] = reply.final;
  std::copy(reply.transaction.begin(), reply.transaction.end(), output.begin() + 8);
  tinta_body_detail::write(output, 24, reply.offset, 4);
  tinta_body_detail::write(output, 28, reply.body.size(), 2);
  if (reply.body.data() != output.data() + LEGACY_BACKUP_REPLY_HEADER_SIZE)
    std::copy(reply.body.begin(), reply.body.end(), output.begin() + LEGACY_BACKUP_REPLY_HEADER_SIZE);
  return true;
}
inline bool decodeLegacyBackupReply(std::span<const uint8_t> input, LegacyBackupReply& output) {
  if (input.size() < LEGACY_BACKUP_REPLY_HEADER_SIZE || input[0] != 'T' || input[1] != 'L' || input[2] != 'S' ||
      input[3] != 1 || input[6] > 1 || input[7] || input[30] || input[31] ||
      tinta_body_detail::read(input, 28, 2) != input.size() - LEGACY_BACKUP_REPLY_HEADER_SIZE)
    return false;
  LegacyBackupReply reply;
  reply.result = static_cast<LegacyBackupResult>(input[4]);
  reply.role = input[5];
  reply.final = input[6];
  std::copy_n(input.begin() + 8, 16, reply.transaction.begin());
  reply.offset = tinta_body_detail::read(input, 24, 4);
  reply.body = input.subspan(LEGACY_BACKUP_REPLY_HEADER_SIZE);
  if (!validLegacyBackupReply(reply)) return false;
  output = reply;
  return true;
}
}  // namespace companion

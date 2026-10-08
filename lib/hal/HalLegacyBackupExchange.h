#pragma once

#include "CompanionLegacyBackupReply.h"
#include "HalLegacyTintaBackupSession.h"

namespace companion {
// Checked heap owner; caller authenticates installation/generation, validates capture course and excludes writers.
class HalLegacyBackupExchange {
 public:
  bool close() {
    bound = fileOpen = false;
    return session.closeExport();
  }
  size_t dispatch(const LegacyBackupRequest& request, const Identity& reader, std::span<uint8_t> output) {
    if (!validLegacyBackupRequest(request) || output.size() < LEGACY_BACKUP_REPLY_HEADER_SIZE) return 0;
    LegacyBackupReply reply;
    reply.transaction = request.transaction;
    reply.role = request.role;
    reply.offset = request.offset;
    if (bound && (request.course != binding.course || request.generation != binding.generation ||
                  request.transaction != binding.transaction || request.bound != binding.bound)) {
      reply.result = LegacyBackupResult::Conflict;
    } else if (request.operation == LegacyBackupOperation::Close) {
      reply.result = session.closeExport() ? LegacyBackupResult::Ok : LegacyBackupResult::IoError;
      reply.final = reply.result == LegacyBackupResult::Ok;
      bound = false;
      fileOpen = false;
    } else {
      if (!bound) {
        if (request.operation != LegacyBackupOperation::Capture &&
            !session.prepareCourse(reader, request.generation, request.course, request.transaction, request.bound,
                                   true))
          return encodeFailure(reply, output);
        binding = request;
        bound = true;
        fileOpen = false;
      }
      bool success = false;
      if (request.operation == LegacyBackupOperation::Capture) {
        success = session.captureCourse(reader, request.generation, request.course, request.transaction, request.bound);
        reply.final = success;
      } else if (request.operation == LegacyBackupOperation::Manifest) {
        if (output.size() < LEGACY_BACKUP_REPLY_HEADER_SIZE + LEGACY_TINTA_BACKUP_MANIFEST_SIZE) return 0;
        const auto body = output.subspan(LEGACY_BACKUP_REPLY_HEADER_SIZE, LEGACY_TINTA_BACKUP_MANIFEST_SIZE);
        success = session.readManifest(body);
        if (success) reply.body = body;
        reply.final = success;
      } else {
        if (output.size() < LEGACY_BACKUP_REPLY_HEADER_SIZE + request.count) return 0;
        if (!fileOpen || fileRole != request.role || nextOffset != request.offset) {
          fileOpen = session.openExport(static_cast<LegacyTintaBackupRole>(request.role), request.offset);
          fileRole = request.role;
        }
        size_t written = 0;
        const auto body = output.subspan(LEGACY_BACKUP_REPLY_HEADER_SIZE, request.count);
        success = fileOpen && session.readExport(request.offset, body, written);
        if (success) {
          reply.body = body.first(written);
          reply.final = session.exportComplete();
          nextOffset = request.offset + written;
        }
        fileOpen = success && !reply.final;
      }
      reply.result = success ? LegacyBackupResult::Ok : LegacyBackupResult::IoError;
    }
    const auto size = LEGACY_BACKUP_REPLY_HEADER_SIZE + reply.body.size();
    return encodeLegacyBackupReply(reply, output.first(size)) ? size : 0;
  }

 private:
  static size_t encodeFailure(LegacyBackupReply& reply, std::span<uint8_t> output) {
    reply.result = LegacyBackupResult::IoError;
    return encodeLegacyBackupReply(reply, output.first(LEGACY_BACKUP_REPLY_HEADER_SIZE))
               ? LEGACY_BACKUP_REPLY_HEADER_SIZE
               : 0;
  }
  HalLegacyTintaBackupSession session;
  LegacyBackupRequest binding;
  uint64_t nextOffset = 0;
  uint8_t fileRole = 0xff;
  bool bound = false, fileOpen = false;
};
}  // namespace companion

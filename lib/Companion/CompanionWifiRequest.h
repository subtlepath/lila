#pragma once

#include "CompanionContentRemovalRequest.h"
#include "CompanionCourseSwitchRequest.h"
#include "CompanionDeclaredTransferCommand.h"
#include "CompanionJournalMergeReadiness.h"
#include "CompanionJournalMergeRequest.h"
#include "CompanionJournalState.h"
#include "CompanionLegacyBackupRequest.h"
#include "CompanionTintaMigrationAdmission.h"
#include "CompanionTransferCommands.h"

namespace companion {
inline bool wifiTransferStateMatches(const TransferState& state, const Identity& transaction, const Identity& owner,
                                     const Identity& generation) {
  return state.transaction == transaction && state.owner == owner && state.storageGeneration == generation;
}
// Separate parsing frames keep the declared command below the stack limit.
[[gnu::noinline]] inline bool validWifiDeclaredBegin(std::span<const uint8_t> body, const Identity& transaction,
                                                     const Identity& owner, const Identity& generation) {
  DeclaredBeginTransferCommand parsed;
  return decodeDeclaredBeginTransfer(body, parsed) &&
         wifiTransferStateMatches(parsed.declaration.state, transaction, owner, generation);
}
[[gnu::noinline]] inline bool validWifiLegacyBegin(std::span<const uint8_t> body, const Identity& transaction,
                                                   const Identity& owner, const Identity& generation) {
  BeginTransferCommand parsed;
  return decodeBeginTransfer(body, parsed) && wifiTransferStateMatches(parsed.state, transaction, owner, generation);
}
[[gnu::noinline]] inline bool validWifiJournalMerge(std::span<const uint8_t> body, const Identity& transaction,
                                                    const Identity& owner, const Identity& generation) {
  JournalMergeRequestView request;
  if (!decodeJournalMergeRequest(body, request) || request.transaction != transaction) return false;
  if (request.operation == JournalMergeOperation::Append) return true;
  JournalMergeIntent declaration;
  return decodeJournalMergeIntent(request.declaration, declaration) && declaration.owner == owner &&
         declaration.generation == generation;
}
[[gnu::noinline]] inline bool decodeWifiRemoval(std::span<const uint8_t> body, ContentRemovalRequest& request) {
  return decodeContentRemovalRequest(body, request);
}
[[gnu::noinline]] inline bool validWifiTintaMigration(std::span<const uint8_t> body, const Identity& transaction,
                                                      const Identity& owner, const Identity& generation) {
  if (body.size() != TINTA_MIGRATION_ADMISSION_SIZE || body[0] != 'T' || body[1] != 'M' || body[2] != 'A' ||
      body[3] != 1 || tinta_body_detail::read(body, 252, 4) != binary_record::crc32(body.data(), 252))
    return false;
  for (const auto field : {body.subspan(140, 16), body.subspan(156, 32), body.subspan(188, 16), body.subspan(204, 16),
                           body.subspan(220, 32)})
    if (!std::any_of(field.begin(), field.end(), [](uint8_t byte) { return byte != 0; })) return false;
  JournalMergeIntent merge;
  return decodeJournalMergeIntent(body.subspan(4, JOURNAL_MERGE_INTENT_SIZE), merge) &&
         merge.merged.count > merge.previous.count && merge.transaction == transaction && merge.owner == owner &&
         merge.generation == generation;
}
[[gnu::noinline]] inline bool validWifiRemoval(std::span<const uint8_t> body, const Identity& transaction,
                                               const Identity& owner, const Identity& generation) {
  ContentRemovalRequest request;
  return decodeWifiRemoval(body, request) && request.transaction == transaction && request.owner == owner &&
         request.generation == generation;
}
inline bool validWifiTransferRequest(const FrameView& request, const Identity& transaction, const Identity& owner,
                                     const Identity& generation) {
  if (request.response || request.payload.size() > MAX_CONTROL_PAYLOAD) return false;
  const auto& body = request.payload;
  if (request.command == Command::RemoveContent) return validWifiRemoval(body, transaction, owner, generation);
  if (request.command == Command::BeginTransfer) {
    if (body.size() >= 2 && body[0] == 1 && body[1] == static_cast<uint8_t>(RecordKind::ContentManifest))
      return validWifiDeclaredBegin(body, transaction, owner, generation);
    return validWifiLegacyBegin(body, transaction, owner, generation);
  }
  if (request.command == Command::TransferChunk) {
    if (body.size() <= CHUNK_HEADER_SIZE) return false;
  } else if (request.command == Command::ExchangeChanges) {
    if (body.size() < transaction.size()) return false;
    if (body.size() == transaction.size() + COURSE_SWITCH_REQUEST_SIZE) {
      CourseSwitchRequest consent;
      if (!decodeCourseSwitchRequest(body.subspan(transaction.size()), consent) || consent.transaction != transaction ||
          consent.generation != generation)
        return false;
    } else if (body.size() == transaction.size() + JOURNAL_MERGE_READINESS_REQUEST_SIZE) {
      Identity requested{};
      JournalMergeSnapshot snapshot;
      if (!decodeJournalMergeReadinessRequest(body.subspan(transaction.size()), requested, snapshot) ||
          requested != generation)
        return false;
    } else if (body.size() == transaction.size() + JOURNAL_STATE_REQUEST_SIZE) {
      Identity requested{};
      if (!decodeJournalStateRequest(body.subspan(transaction.size()), requested) || requested != generation)
        return false;
    } else if (body.size() == transaction.size() + LEGACY_BACKUP_REQUEST_SIZE) {
      LegacyBackupRequest backup;
      if (!decodeLegacyBackupRequest(body.subspan(transaction.size()), backup) || backup.transaction != transaction ||
          backup.generation != generation)
        return false;
    } else if (body.size() == transaction.size() + TINTA_MIGRATION_ADMISSION_SIZE && body[16] == 'T' &&
               body[17] == 'M' && body[18] == 'A') {
      if (!validWifiTintaMigration(body.subspan(transaction.size()), transaction, owner, generation)) return false;
    } else if (body.size() != transaction.size() + JOURNAL_EXPORT_REQUEST_SIZE &&
               !validWifiJournalMerge(body.subspan(transaction.size()), transaction, owner, generation))
      return false;
  } else if (request.command == Command::TransferStatus || request.command == Command::Commit ||
             request.command == Command::Abort || request.command == Command::JournalFormats) {
    if (body.size() != transaction.size()) return false;
  } else {
    return false;
  }
  return std::equal(transaction.begin(), transaction.end(), body.begin());
}
}  // namespace companion

#pragma once

#include "CompanionJournalMergeRequest.h"
#include "HalJournalMergeCandidateSession.h"

namespace companion {
// Checked heap workspace; caller authenticates the transport and excludes other journal writers.
class HalJournalMergeReceiveSession {
 public:
  TintaJournalResult dispatch(std::span<const uint8_t> payload, const Identity& owner, const Identity& generation,
                              const Identity* course, TintaSubjectCatalog* catalog) {
    JournalMergeRequestView request;
    if (!decodeJournalMergeRequest(payload, request)) return TintaJournalResult::Invalid;
    if (request.operation == JournalMergeOperation::Append) {
      if (!bound || completed) return TintaJournalResult::Unavailable;
      if (owner != active.owner || generation != active.generation || request.transaction != active.transaction)
        return TintaJournalResult::Conflict;
      if (!decodeRecord(request.envelope, event)) return TintaJournalResult::Invalid;
      return candidate.append(event, request.body);
    }
    if (!decodeJournalMergeIntent(request.declaration, decoded) || decoded.owner != owner ||
        decoded.generation != generation)
      return TintaJournalResult::Conflict;
    if (bound && decoded != active) return TintaJournalResult::Conflict;
    if (request.operation == JournalMergeOperation::Abort) {
      const auto result = candidate.abort(decoded, generation);
      if (result == TintaJournalResult::Ok) bound = completed = false;
      return result;
    }
    if (request.operation == JournalMergeOperation::Begin) {
      const auto result = candidate.begin(decoded, generation);
      if (result == TintaJournalResult::Ok || result == TintaJournalResult::Duplicate) {
        active = decoded;
        bound = true;
        completed = result == TintaJournalResult::Duplicate;
      } else {
        bound = completed = false;
      }
      return result;
    }
    if (!bound) return TintaJournalResult::Unavailable;
    if (completed) return TintaJournalResult::Duplicate;
    const auto result = candidate.commit(generation, course, catalog);
    if (result == TintaJournalResult::Ok) completed = true;
    return result;
  }
  bool close() {
    bound = completed = false;
    return candidate.close();
  }
  uint32_t count() const { return completed ? active.merged.count : candidate.count(); }
  bool hasBinding() const { return bound; }
  const JournalMergeIntent* receivingDeclaration() const {
    return bound && !completed && candidate.available() ? &active : nullptr;
  }

 private:
  HalJournalMergeCandidateSession candidate;
  JournalMergeIntent active, decoded;
  SyncEvent event;
  bool bound = false, completed = false;
};
}  // namespace companion

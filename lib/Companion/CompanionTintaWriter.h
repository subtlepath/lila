#pragma once

#include "CompanionIdentity.h"
#include "CompanionTintaJournal.h"

namespace companion {

// Complete audited heads of the writer's initial journal. Retain the snapshot
// independently of the live journal so appended preference batches cannot change it.
// Reads are sequential, starting at zero; an SD source need not support rewinding.
class PreferenceKnowledgeHeads {
 public:
  virtual ~PreferenceKnowledgeHeads() = default;
  virtual uint32_t count() const = 0;
  virtual bool read(uint32_t at, EventIdentity& output) = 0;
};

// Session-owned: the event member exceeds the task's local-variable budget.
class TintaWriter {
 public:
  TintaWriter(TintaJournal& journal, TintaJournalStorage& storage) : journal(journal), storage(storage) {}
  TintaJournalResult start(IdentityStorage& identities) {
    ready = false;
    exhausted = false;
    hasFrontier = false;
    const auto recovery = journal.open();
    if (recovery != TintaJournalResult::Ok) return recovery;
    if (journal.count() != 0) {
      const auto result = journal.read(journal.count() - 1);
      if (result != TintaJournalResult::Ok) return result;
      frontier = journal.event().identity;
      hasFrontier = true;
    }
    const auto reserved = provisionIdentity(identities, identity);
    if (reserved != IdentityResult::Ok)
      return reserved == IdentityResult::Exhausted ? TintaJournalResult::Exhausted
             : reserved == IdentityResult::IoError ? TintaJournalResult::IoError
                                                   : TintaJournalResult::Corrupt;
    const auto unused = journal.checkUnusedEpoch(identity.device, identity.eventEpoch);
    if (unused != TintaJournalResult::Ok) return unused;
    committedCount = journal.count();
    sequence = 1;
    ready = true;
    return TintaJournalResult::Ok;
  }

  // Only a successful durable result permits the caller to update derived learner state.
  TintaJournalResult record(const TintaBody& body, const Digest& resource, uint32_t studyDay, uint64_t timestamp,
                            ClockQuality quality) {
    if (exhausted) return TintaJournalResult::Exhausted;
    if (!ready) return TintaJournalResult::Unavailable;
    if (body.kind == EventKind::UndoReview) {
      const auto result = validateUndo(body);
      if (result != TintaJournalResult::Ok) return result;
    }
    const size_t length = encodeTintaBody(body, bytes);
    if (length == 0) return TintaJournalResult::Invalid;
    candidate.identity = {identity.device, identity.eventEpoch, sequence};
    candidate.storageGeneration = identity.storageGeneration;
    candidate.kind = body.kind;
    candidate.resource = resource;
    candidate.studyDay = studyDay;
    candidate.timestamp = timestamp;
    candidate.clockQuality = quality;
    candidate.schedulerVersion = 0;
    candidate.schedulerConfiguration.fill(0);
    for (auto& ancestor : candidate.ancestors) ancestor = {};
    candidate.ancestorCount = hasFrontier ? 1 : 0;
    if (hasFrontier) candidate.ancestors[0] = frontier;
    if (body.kind == EventKind::UndoReview && (!hasFrontier || frontier != body.undoTarget))
      candidate.ancestors[candidate.ancestorCount++] = body.undoTarget;
    if (!storage.digest(std::span(bytes).first(length), candidate.bodyHash)) return fail(TintaJournalResult::IoError);
    if (body.kind == EventKind::Review) {
      std::array<uint8_t, 6> configuration{};
      encodeTintaConfiguration(body.configuration, configuration);
      candidate.schedulerVersion = 1;
      if (!storage.digest(configuration, candidate.schedulerConfiguration)) return fail(TintaJournalResult::IoError);
    }
    return commitCandidate(std::span(bytes).first(length));
  }
  TintaJournalResult recordPreference(std::span<const uint8_t> encoded, uint32_t studyDay, uint64_t timestamp,
                                      ClockQuality quality) {
    if (exhausted) return TintaJournalResult::Exhausted;
    if (!ready) return TintaJournalResult::Unavailable;
    PreferenceBodyView preference;
    if (!decodePreferenceBody(encoded, preference)) return TintaJournalResult::Invalid;
    candidate = {};
    candidate.identity = {identity.device, identity.eventEpoch, sequence};
    candidate.storageGeneration = identity.storageGeneration;
    candidate.kind = EventKind::Preference;
    candidate.resource = PREFERENCE_SCOPE;
    candidate.studyDay = studyDay;
    candidate.timestamp = timestamp;
    candidate.clockQuality = quality;
    candidate.ancestorCount = hasFrontier ? 1 : 0;
    if (hasFrontier) candidate.ancestors[0] = frontier;
    if (!storage.digest(encoded, candidate.bodyHash)) return fail(TintaJournalResult::IoError);
    return commitCandidate(encoded);
  }
  // One actual preference edit may require multiple envelopes. Identical values
  // and implicit sequence predecessors let the final event cover every known head.
  TintaJournalResult recordPreferenceResolving(std::span<const uint8_t> encoded, PreferenceKnowledgeHeads& heads,
                                               uint32_t studyDay, uint64_t timestamp, ClockQuality quality) {
    if (exhausted) return TintaJournalResult::Exhausted;
    if (!ready) return TintaJournalResult::Unavailable;
    PreferenceBodyView preference;
    if (!decodePreferenceBody(encoded, preference)) return TintaJournalResult::Invalid;
    return recordResolving(encoded, EventKind::Preference, PREFERENCE_SCOPE, heads, studyDay, timestamp, quality);
  }
  TintaJournalResult recordReadingPosition(std::span<const uint8_t> encoded, const Digest& resource,
                                           PreferenceKnowledgeHeads& heads, uint64_t timestamp, ClockQuality quality) {
    if (exhausted) return TintaJournalResult::Exhausted;
    if (!ready) return TintaJournalResult::Unavailable;
    ReadingAnchor anchor;
    if (!decodeReadingAnchor(encoded, anchor) || !tinta_body_detail::nonzero(resource))
      return TintaJournalResult::Invalid;
    return recordResolving(encoded, EventKind::ReadingPosition, resource, heads, 0, timestamp, quality);
  }

  TintaJournalResult recordBookmark(std::span<const uint8_t> encoded, const Digest& resource,
                                    PreferenceKnowledgeHeads& heads, uint64_t timestamp, ClockQuality quality) {
    if (exhausted) return TintaJournalResult::Exhausted;
    if (!ready) return TintaJournalResult::Unavailable;
    BookmarkBodyView bookmark;
    if (!decodeBookmarkBody(encoded, bookmark) || !tinta_body_detail::nonzero(resource))
      return TintaJournalResult::Invalid;
    return recordResolving(encoded, bookmark.deleted ? EventKind::BookmarkDelete : EventKind::BookmarkPut, resource,
                           heads, 0, timestamp, quality);
  }

 private:
  TintaJournalResult recordResolving(std::span<const uint8_t> encoded, EventKind kind, const Digest& resource,
                                     PreferenceKnowledgeHeads& heads, uint32_t studyDay, uint64_t timestamp,
                                     ClockQuality quality) {
    const auto count = heads.count();
    if (!count) {
      candidate = {};
      candidate.identity = {identity.device, identity.eventEpoch, sequence};
      candidate.storageGeneration = identity.storageGeneration;
      candidate.kind = kind;
      candidate.resource = resource;
      candidate.studyDay = studyDay;
      candidate.timestamp = timestamp;
      candidate.clockQuality = quality;
      candidate.ancestorCount = hasFrontier ? 1 : 0;
      if (hasFrontier) candidate.ancestors[0] = frontier;
      if (!storage.digest(encoded, candidate.bodyHash)) return fail(TintaJournalResult::IoError);
      return commitCandidate(encoded);
    }
    if (journal.count() != committedCount) return fail(TintaJournalResult::Conflict);
    for (uint32_t at = 0; at < count;) {
      if (exhausted) return fail(TintaJournalResult::Exhausted);
      candidate = {};
      candidate.identity = {identity.device, identity.eventEpoch, sequence};
      candidate.storageGeneration = identity.storageGeneration;
      candidate.kind = kind;
      candidate.resource = resource;
      candidate.studyDay = studyDay;
      candidate.timestamp = timestamp;
      candidate.clockQuality = quality;
      while (candidate.ancestorCount < MAX_ANCESTORS && at < count) {
        if (!heads.read(at++, candidate.ancestors[candidate.ancestorCount])) return fail(TintaJournalResult::IoError);
        ++candidate.ancestorCount;
      }
      if (heads.count() != count) return fail(TintaJournalResult::Conflict);
      if (!storage.digest(encoded, candidate.bodyHash)) return fail(TintaJournalResult::IoError);
      const auto result = commitCandidate(encoded);
      if (result != TintaJournalResult::Ok) return fail(result);
    }
    return TintaJournalResult::Ok;
  }

 public:
  TintaJournalResult recordFlags(std::span<const TintaBody, 2> bodies, const Digest& resource, uint32_t studyDay,
                                 uint64_t timestamp, ClockQuality quality) {
    if (exhausted || sequence == UINT64_MAX) return TintaJournalResult::Exhausted;
    if (!ready) return TintaJournalResult::Unavailable;
    if (bodies[0].kind != EventKind::Suspension || bodies[1].kind != EventKind::Star ||
        bodies[0].course != bodies[1].course || bodies[0].uid != bodies[1].uid)
      return TintaJournalResult::Invalid;
    if (journal.count() != committedCount) return fail(TintaJournalResult::Conflict);
    std::array<std::span<const uint8_t>, 2> encoded;
    for (size_t at = 0; at < 2; ++at) {
      const auto length = encodeTintaBody(bodies[at], pairBytes[at]);
      if (!length) return TintaJournalResult::Invalid;
      encoded[at] = std::span(pairBytes[at]).first(length);
      auto& event = pairEvents[at];
      event = {};
      event.identity = {identity.device, identity.eventEpoch, sequence + at};
      event.storageGeneration = identity.storageGeneration;
      event.kind = bodies[at].kind;
      event.resource = resource;
      event.studyDay = studyDay;
      event.timestamp = timestamp;
      event.clockQuality = quality;
      event.ancestorCount = at || hasFrontier ? 1 : 0;
      if (event.ancestorCount) event.ancestors[0] = at ? pairEvents[0].identity : frontier;
      if (!storage.digest(encoded[at], event.bodyHash)) return fail(TintaJournalResult::IoError);
    }
    const auto result = journal.appendPair(pairEvents, encoded);
    if (result != TintaJournalResult::Ok) return fail(result);
    frontier = pairEvents[1].identity;
    hasFrontier = true;
    committedCount += 2;
    if (sequence == UINT64_MAX - 1)
      exhausted = true;
    else
      sequence += 2;
    return result;
  }
  const EventIdentity& committedIdentity() const { return frontier; }
  bool available() const { return ready && !exhausted; }
  void stop() { ready = false; }

 private:
  TintaJournalResult commitCandidate(std::span<const uint8_t> encoded) {
    const auto result = journal.appendFresh(candidate, encoded, committedCount);
    if (result == TintaJournalResult::Ok || result == TintaJournalResult::Duplicate) {
      frontier = candidate.identity;
      ++committedCount;
      hasFrontier = true;
      if (sequence == UINT64_MAX)
        exhausted = true;
      else
        ++sequence;
      return result;
    }
    return result == TintaJournalResult::Invalid ? result : fail(result);
  }
  TintaJournalResult validateUndo(const TintaBody& body) {
    for (uint32_t remaining = journal.count(); remaining != 0; --remaining) {
      const auto result = journal.read(remaining - 1);
      if (result != TintaJournalResult::Ok) return fail(result);
      if (journal.event().identity != body.undoTarget) continue;
      if (journal.event().kind != EventKind::Review || !decodeTintaBody(journal.body(), undoReviewBody) ||
          undoReviewBody.kind != EventKind::Review || undoReviewBody.course != body.course ||
          undoReviewBody.uid != body.uid)
        return TintaJournalResult::Invalid;
      return TintaJournalResult::Ok;
    }
    return TintaJournalResult::Invalid;
  }
  TintaJournalResult fail(TintaJournalResult result) {
    ready = false;
    return result;
  }
  TintaJournal& journal;
  TintaJournalStorage& storage;
  IdentityState identity;
  EventIdentity frontier;
  SyncEvent candidate;
  std::array<SyncEvent, 2> pairEvents;
  std::array<std::array<uint8_t, MAX_TINTA_BODY_SIZE>, 2> pairBytes{};
  TintaBody undoReviewBody;
  std::array<uint8_t, MAX_TINTA_BODY_SIZE> bytes{};
  uint64_t sequence = 1;
  uint32_t committedCount = 0;
  bool ready = false;
  bool exhausted = false;
  bool hasFrontier = false;
};

}  // namespace companion

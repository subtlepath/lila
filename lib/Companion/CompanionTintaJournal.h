#pragma once

#include "CompanionReadingBody.h"
#include "CompanionTintaBody.h"

namespace companion {

// Successful writes/truncations are durable. Header slots occupy separate files/sectors.
class TintaJournalStorage {
 public:
  virtual ~TintaJournalStorage() = default;
  virtual bool size(uint32_t& bytes) = 0;
  virtual bool read(uint32_t offset, std::span<uint8_t> bytes) = 0;
  virtual bool write(uint32_t offset, std::span<const uint8_t> bytes) = 0;
  virtual bool truncate(uint32_t bytes) = 0;
  // A missing header returns length zero; I/O errors return false.
  virtual bool readHeader(uint8_t slot, std::span<uint8_t> bytes, size_t& length) = 0;
  virtual bool writeHeader(uint8_t slot, std::span<const uint8_t> bytes) = 0;
  virtual bool digest(std::span<const uint8_t> bytes, Digest& output) = 0;
};

enum class TintaJournalResult : uint8_t { Ok, Duplicate, Invalid, Conflict, Corrupt, IoError, Exhausted, Unavailable };

// Own this object outside the small task stack; scratch borrows the session workspace.
class TintaJournal {
 public:
  static constexpr size_t RECORD_SIZE = 512;
  static constexpr size_t EXTENDED_RECORD_SIZE = 1024;
  static constexpr size_t HEADER_SIZE = 64;
  static constexpr size_t MAX_BODY_SIZE = MAX_BOOKMARK_BODY_SIZE;
  static_assert(12 + MAX_RECORD_SIZE + MAX_BODY_SIZE <= EXTENDED_RECORD_SIZE - 4);
  explicit TintaJournal(TintaJournalStorage& storage, std::span<uint8_t> scratch)
      : storage(storage),
        scratch(scratch.size() >= RECORD_SIZE
                    ? scratch.first(scratch.size() >= EXTENDED_RECORD_SIZE ? EXTENDED_RECORD_SIZE : RECORD_SIZE)
                    : std::span<uint8_t>{}) {}

  TintaJournalResult open() {
    ready = false;
    envelopeLength = bodyLength = 0;
    if (scratch.empty()) return TintaJournalResult::Unavailable;
    uint32_t bytes = 0;
    if (!storage.size(bytes)) return TintaJournalResult::IoError;
    Header best;
    bool found = false;
    bool present = false;
    for (uint8_t slot = 0; slot < 2; ++slot) {
      std::array<uint8_t, HEADER_SIZE> buffer{};
      size_t length = 0;
      if (!storage.readHeader(slot, buffer, length)) return TintaJournalResult::IoError;
      present |= length != 0;
      Header candidate;
      if (decodeHeader(buffer, length, slot, candidate)) {
        if (found) {
          const auto& newer = candidate.sequence > best.sequence ? candidate : best;
          const auto& older = candidate.sequence > best.sequence ? best : candidate;
          if (newer.recordSize != older.recordSize) return TintaJournalResult::Corrupt;
          if (newer.sequence - older.sequence != 1 || newer.count <= older.count || newer.count - older.count > 2)
            return TintaJournalResult::Corrupt;
        }
        if (!found || candidate.sequence > best.sequence) best = candidate;
        found = true;
      }
    }
    if (!found) {
      if (present || bytes != 0) return TintaJournalResult::Corrupt;
      best.sequence = 1;
      best.recordSize = scratch.size();
      if (!persistHeader(best)) return TintaJournalResult::IoError;
    }
    if (scratch.size() < best.recordSize) return TintaJournalResult::Unavailable;
    if (best.count > UINT32_MAX / best.recordSize || bytes < best.count * best.recordSize)
      return TintaJournalResult::Corrupt;
    header = best;
    for (uint32_t index = 0; index < header.count; ++index) {
      const auto result = load(index);
      if (result != TintaJournalResult::Ok) return result;
    }
    const uint32_t committedBytes = header.count * header.recordSize;
    if (bytes != committedBytes && !storage.truncate(committedBytes)) return TintaJournalResult::IoError;
    ready = true;
    return TintaJournalResult::Ok;
  }

  static uint8_t headerVersion(std::span<const uint8_t> bytes, uint8_t slot) {
    Header decoded;
    return slot < 2 && decodeHeader(bytes, bytes.size(), slot, decoded) ? bytes[3] : 0;
  }
  uint32_t count() const { return ready ? header.count : 0; }
  bool available() const { return ready; }
  uint16_t recordSize() const { return ready ? header.recordSize : 0; }

  TintaJournalResult append(const SyncEvent& event, std::span<const uint8_t> body) {
    return appendImpl(event, body, true);
  }

  // Duplicate delivery remains valid at the limit; a new event cannot exceed it.
  TintaJournalResult appendBounded(const SyncEvent& event, std::span<const uint8_t> body, uint32_t maximumCount) {
    if (!ready) return TintaJournalResult::Unavailable;
    if (maximumCount > UINT32_MAX / header.recordSize) return TintaJournalResult::Invalid;
    if (header.count > maximumCount) return TintaJournalResult::Conflict;
    return appendImpl(event, body, true, maximumCount);
  }

  // Exactly two independent caller-owned events commit through one header.
  TintaJournalResult appendPair(std::span<const SyncEvent, 2> events,
                                std::span<const std::span<const uint8_t>, 2> bodies) {
    if (!ready) return TintaJournalResult::Unavailable;
    if (events[0].identity == events[1].identity) return TintaJournalResult::Invalid;
    uint8_t duplicates = 0;
    std::array<bool, 2> closed{};
    for (size_t at = 0; at < 2; ++at) {
      if (&events[at] == &decodedEvent || overlaps(bodies[at]) || !bodyFits(bodies[at]))
        return TintaJournalResult::Invalid;
      auto result = validate(events[at], bodies[at]);
      if (result != TintaJournalResult::Ok) return result;
      bool previous = events[at].identity.sequence == 1;
      uint8_t ancestors = 0;
      if (at) markDependencies(events[at], events[0].identity, previous, ancestors);
      for (uint32_t index = 0; index < header.count; ++index) {
        result = load(index);
        if (result != TintaJournalResult::Ok) return fail(result);
        markDependencies(events[at], decodedEvent.identity, previous, ancestors);
        if (decodedEvent.identity != events[at].identity) continue;
        if (!sameEvent(decodedEvent, events[at]) || bodyLength != bodies[at].size() ||
            !std::equal(bodies[at].begin(), bodies[at].end(), scratch.begin() + 12 + envelopeLength))
          return TintaJournalResult::Conflict;
        ++duplicates;
        break;
      }
      closed[at] = previous && ancestors == static_cast<uint8_t>((1u << events[at].ancestorCount) - 1);
    }
    if (duplicates == 2) return TintaJournalResult::Duplicate;
    if (duplicates) return TintaJournalResult::Conflict;
    if (!closed[0] || !closed[1]) return TintaJournalResult::Invalid;
    if (header.sequence == UINT64_MAX || header.count > UINT32_MAX / header.recordSize - 2)
      return TintaJournalResult::Exhausted;
    for (uint32_t at = 0; at < 2; ++at) {
      const auto result = writeRecord(events[at], bodies[at], header.count + at);
      if (result != TintaJournalResult::Ok) return fail(result);
    }
    Header next{header.sequence + 1, header.count + 2, header.recordSize};
    if (!persistHeader(next)) return fail(TintaJournalResult::IoError);
    header = next;
    return TintaJournalResult::Ok;
  }

 private:
  friend class TintaWriter;
  friend class JournalMigration;
  friend class JournalMergeRetentionProof;
  TintaJournalResult checkUnusedEpoch(const Identity& origin, uint64_t epoch) {
    if (!ready) return TintaJournalResult::Unavailable;
    for (uint32_t index = 0; index < header.count; ++index) {
      const auto result = load(index);
      if (result != TintaJournalResult::Ok) return fail(result);
      if (decodedEvent.identity.origin == origin && decodedEvent.identity.epoch == epoch)
        return TintaJournalResult::Conflict;
    }
    return TintaJournalResult::Ok;
  }
  TintaJournalResult appendFresh(const SyncEvent& event, std::span<const uint8_t> body, uint32_t expectedCount) {
    if (!ready) return TintaJournalResult::Unavailable;
    if (header.count != expectedCount) return TintaJournalResult::Conflict;
    return appendImpl(event, body, false);
  }
  TintaJournalResult appendImpl(const SyncEvent& event, std::span<const uint8_t> body, bool checkDuplicates,
                                uint32_t maximumCount = UINT32_MAX) {
    if (!ready) return TintaJournalResult::Unavailable;
    if (&event == &decodedEvent || overlaps(body) || !bodyFits(body)) return TintaJournalResult::Invalid;
    auto result = validate(event, body);
    if (result != TintaJournalResult::Ok) return result;
    bool previous = event.identity.sequence == 1;
    uint8_t ancestors = 0;
    for (uint32_t index = 0; checkDuplicates && index < header.count; ++index) {
      result = load(index);
      if (result != TintaJournalResult::Ok) return fail(result);
      markDependencies(event, decodedEvent.identity, previous, ancestors);
      if (decodedEvent.identity == event.identity) {
        if (!sameEvent(decodedEvent, event) || bodyLength != body.size() ||
            !std::equal(body.begin(), body.end(), scratch.begin() + 12 + envelopeLength))
          return TintaJournalResult::Conflict;
        return TintaJournalResult::Duplicate;
      }
    }
    if (checkDuplicates && (!previous || ancestors != static_cast<uint8_t>((1u << event.ancestorCount) - 1)))
      return TintaJournalResult::Invalid;
    if (header.sequence == UINT64_MAX || header.count >= UINT32_MAX / header.recordSize || header.count >= maximumCount)
      return TintaJournalResult::Exhausted;
    result = writeRecord(event, body, header.count);
    if (result != TintaJournalResult::Ok) return fail(result);
    Header next{header.sequence + 1, header.count + 1, header.recordSize};
    if (!persistHeader(next)) return fail(TintaJournalResult::IoError);
    header = next;
    return TintaJournalResult::Ok;
  }
  TintaJournalResult writeRecord(const SyncEvent& event, std::span<const uint8_t> body, uint32_t index) {
    envelopeLength = bodyLength = 0;
    std::fill(scratch.begin(), scratch.end(), 0);
    scratch[0] = 'T';
    scratch[1] = 'J';
    scratch[2] = 'E';
    scratch[3] = header.recordSize == EXTENDED_RECORD_SIZE ? 2 : 1;
    const size_t encoded = encodeRecord(event, scratch.subspan(12, MAX_RECORD_SIZE));
    if (encoded == 0) return TintaJournalResult::Invalid;
    tinta_body_detail::write(scratch, 4, encoded, 2);
    tinta_body_detail::write(scratch, 6, body.size(), 2);
    std::copy(body.begin(), body.end(), scratch.begin() + 12 + encoded);
    tinta_body_detail::write(scratch, header.recordSize - 4, crc(scratch.first(header.recordSize - 4)), 4);
    return storage.write(index * header.recordSize, scratch.first(header.recordSize)) ? TintaJournalResult::Ok
                                                                                      : TintaJournalResult::IoError;
  }

 public:
  // Decoded storage and the body view remain valid only until the next operation.
  TintaJournalResult read(uint32_t index) {
    if (!ready) return TintaJournalResult::Unavailable;
    if (index >= header.count) return TintaJournalResult::Invalid;
    const auto result = load(index);
    return result == TintaJournalResult::Ok ? result : fail(result);
  }
  const SyncEvent& event() const { return decodedEvent; }
  std::span<const uint8_t> body() const {
    return ready && envelopeLength >= SYNC_EVENT_BASE_SIZE ? scratch.subspan(12 + envelopeLength, bodyLength)
                                                           : std::span<const uint8_t>{};
  }

 private:
  static void markDependencies(const SyncEvent& event, const EventIdentity& identity, bool& previous,
                               uint8_t& ancestors) {
    if (event.identity.origin == identity.origin && event.identity.epoch == identity.epoch &&
        event.identity.sequence > 1 && identity.sequence == event.identity.sequence - 1)
      previous = true;
    for (unsigned at = 0; at < event.ancestorCount; ++at)
      if (event.ancestors[at] == identity) ancestors |= static_cast<uint8_t>(1u << at);
  }
  static bool sameEvent(const SyncEvent& lhs, const SyncEvent& rhs) {
    return lhs.identity == rhs.identity && lhs.storageGeneration == rhs.storageGeneration && lhs.kind == rhs.kind &&
           lhs.resource == rhs.resource && lhs.bodyHash == rhs.bodyHash && lhs.studyDay == rhs.studyDay &&
           lhs.timestamp == rhs.timestamp && lhs.clockQuality == rhs.clockQuality &&
           lhs.schedulerVersion == rhs.schedulerVersion && lhs.schedulerConfiguration == rhs.schedulerConfiguration &&
           lhs.ancestorCount == rhs.ancestorCount &&
           std::equal(lhs.ancestors.begin(), lhs.ancestors.begin() + lhs.ancestorCount, rhs.ancestors.begin());
  }
  struct Header {
    uint64_t sequence = 0;
    uint32_t count = 0;
    uint16_t recordSize = RECORD_SIZE;
  };
  static uint32_t crc(std::span<const uint8_t> bytes) {
    uint32_t value = UINT32_MAX;
    for (const uint8_t byte : bytes) {
      value ^= byte;
      for (uint8_t bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xEDB88320U & (0U - (value & 1U)));
    }
    return ~value;
  }
  static bool decodeHeader(std::span<const uint8_t> bytes, size_t length, uint8_t slot, Header& output) {
    if (length != HEADER_SIZE || bytes[0] != 'T' || bytes[1] != 'J' || bytes[2] != 'H' ||
        (bytes[3] != 1 && bytes[3] != 2 && bytes[3] != 3) ||
        crc(bytes.first(60)) != tinta_body_detail::read(bytes, 60, 4) || (bytes[3] == 3 && bytes[16] != 10) ||
        std::any_of(bytes.begin() + (bytes[3] == 3 ? 17 : 16), bytes.begin() + 60,
                    [](uint8_t byte) { return byte != 0; }))
      return false;
    output.recordSize = bytes[3] == 3 ? EXTENDED_RECORD_SIZE : RECORD_SIZE;
    output.sequence = tinta_body_detail::read(bytes, 4, 8);
    output.count = static_cast<uint32_t>(tinta_body_detail::read(bytes, 12, 4));
    const uint64_t count = output.count;
    const bool validCount =
        bytes[3] == 1 ? output.sequence == count + 1
                      : output.sequence > 0 && output.sequence <= count + 1 && count <= 2 * (output.sequence - 1);
    return validCount && (output.sequence & 1) == slot;
  }
  bool persistHeader(const Header& value) {
    std::array<uint8_t, HEADER_SIZE> bytes{};
    bytes[0] = 'T';
    bytes[1] = 'J';
    bytes[2] = 'H';
    bytes[3] = value.recordSize == EXTENDED_RECORD_SIZE ? 3 : 2;
    if (bytes[3] == 3) bytes[16] = 10;
    tinta_body_detail::write(bytes, 4, value.sequence, 8);
    tinta_body_detail::write(bytes, 12, value.count, 4);
    tinta_body_detail::write(bytes, 60, crc(std::span(bytes).first(60)), 4);
    return storage.writeHeader(value.sequence & 1, bytes);
  }
  bool bodyFits(std::span<const uint8_t> bytes) const {
    return bytes.size() <= MAX_BODY_SIZE && 12 + MAX_RECORD_SIZE + bytes.size() <= size_t{header.recordSize} - 4;
  }
  bool overlaps(std::span<const uint8_t> bytes) const {
    if (bytes.empty()) return false;
    const auto start = reinterpret_cast<uintptr_t>(scratch.data());
    const auto input = reinterpret_cast<uintptr_t>(bytes.data());
    return input >= start ? input - start < scratch.size() : start - input < bytes.size();
  }
  TintaJournalResult validate(const SyncEvent& event, std::span<const uint8_t> body) {
    Digest bodyDigest{}, configurationDigest{};
    std::array<uint8_t, 6> configuration{};
    if (event.kind <= EventKind::Preference) {
      PreferenceBodyView preference;
      ReadingAnchor anchor;
      BookmarkBodyView bookmark;
      if (!tinta_body_detail::validIdentity(event.identity) || !tinta_body_detail::nonzero(event.storageGeneration) ||
          event.ancestorCount > MAX_ANCESTORS)
        return TintaJournalResult::Invalid;
      for (unsigned at = 0; at < event.ancestorCount; ++at) {
        const auto& ancestor = event.ancestors[at];
        if (!tinta_body_detail::validIdentity(ancestor) || ancestor == event.identity ||
            (ancestor.origin == event.identity.origin && ancestor.epoch == event.identity.epoch &&
             ancestor.sequence >= event.identity.sequence))
          return TintaJournalResult::Invalid;
        for (unsigned previous = 0; previous < at; ++previous)
          if (event.ancestors[previous] == ancestor) return TintaJournalResult::Invalid;
      }
      const bool validBody = event.kind == EventKind::Preference
                                 ? decodePreferenceBody(body, preference) && event.resource == PREFERENCE_SCOPE
                             : event.kind == EventKind::ReadingPosition
                                 ? decodeReadingAnchor(body, anchor) && tinta_body_detail::nonzero(event.resource)
                                 : decodeBookmarkBody(body, bookmark) && body[1] == static_cast<uint8_t>(event.kind) &&
                                       tinta_body_detail::nonzero(event.resource);
      if (!validBody || event.schedulerVersion != 0 ||
          std::any_of(event.schedulerConfiguration.begin(), event.schedulerConfiguration.end(),
                      [](uint8_t byte) { return byte != 0; }))
        return TintaJournalResult::Invalid;
      if (!storage.digest(body, bodyDigest)) return TintaJournalResult::IoError;
      return event.bodyHash == bodyDigest ? TintaJournalResult::Ok : TintaJournalResult::Invalid;
    }
    if (!decodeTintaBody(body, decodedBody)) return TintaJournalResult::Invalid;
    if (!storage.digest(body, bodyDigest)) return TintaJournalResult::IoError;
    if (decodedBody.kind == EventKind::Review) {
      if (encodeTintaConfiguration(decodedBody.configuration, configuration) == 0) return TintaJournalResult::Invalid;
      if (!storage.digest(configuration, configurationDigest)) return TintaJournalResult::IoError;
    }
    return validateTintaEnvelope(event, decodedBody, bodyDigest, configurationDigest) ? TintaJournalResult::Ok
                                                                                      : TintaJournalResult::Invalid;
  }
  TintaJournalResult load(uint32_t index) {
    if (!storage.read(index * header.recordSize, scratch.first(header.recordSize))) return TintaJournalResult::IoError;
    if (scratch[0] != 'T' || scratch[1] != 'J' || scratch[2] != 'E' ||
        scratch[3] != (header.recordSize == EXTENDED_RECORD_SIZE ? 2 : 1) ||
        tinta_body_detail::read(scratch, header.recordSize - 4, 4) != crc(scratch.first(header.recordSize - 4)) ||
        std::any_of(scratch.begin() + 8, scratch.begin() + 12, [](uint8_t byte) { return byte != 0; }))
      return TintaJournalResult::Corrupt;
    envelopeLength = tinta_body_detail::read(scratch, 4, 2);
    bodyLength = tinta_body_detail::read(scratch, 6, 2);
    if (envelopeLength < SYNC_EVENT_BASE_SIZE || envelopeLength > MAX_RECORD_SIZE || bodyLength > MAX_BODY_SIZE ||
        12 + envelopeLength + bodyLength > size_t{header.recordSize} - 4 ||
        !decodeRecord(scratch.subspan(12, envelopeLength), decodedEvent) ||
        std::any_of(scratch.begin() + 12 + envelopeLength + bodyLength, scratch.begin() + header.recordSize - 4,
                    [](uint8_t byte) { return byte != 0; }))
      return TintaJournalResult::Corrupt;
    const auto result = validate(decodedEvent, scratch.subspan(12 + envelopeLength, bodyLength));
    return result == TintaJournalResult::Invalid ? TintaJournalResult::Corrupt : result;
  }
  TintaJournalResult fail(TintaJournalResult result) {
    ready = false;
    return result;
  }
  TintaJournalStorage& storage;
  std::span<uint8_t> scratch;
  Header header;
  SyncEvent decodedEvent;
  TintaBody decodedBody;
  size_t envelopeLength = 0;
  size_t bodyLength = 0;
  bool ready = false;
};

}  // namespace companion

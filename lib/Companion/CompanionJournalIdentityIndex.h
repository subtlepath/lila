#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionJournalCausalValidation.h"

namespace companion {
inline constexpr size_t JOURNAL_IDENTITY_ENTRY_SIZE = 40;
inline constexpr size_t JOURNAL_IDENTITY_HEADER_SIZE = 16;
struct JournalIdentityEntry {
  EventIdentity identity{};
  uint32_t record = 0;
};
inline bool journalIdentityBefore(const EventIdentity& a, const EventIdentity& b) {
  if (a.origin != b.origin)
    return std::lexicographical_compare(a.origin.begin(), a.origin.end(), b.origin.begin(), b.origin.end());
  if (a.epoch != b.epoch) return a.epoch < b.epoch;
  return a.sequence < b.sequence;
}
inline bool encodeJournalIdentityEntry(const JournalIdentityEntry& entry, std::span<uint8_t> bytes) {
  if (bytes.size() != JOURNAL_IDENTITY_ENTRY_SIZE || !tinta_body_detail::validIdentity(entry.identity)) return false;
  std::copy(entry.identity.origin.begin(), entry.identity.origin.end(), bytes.begin());
  tinta_body_detail::write(bytes, 16, entry.identity.epoch, 8);
  tinta_body_detail::write(bytes, 24, entry.identity.sequence, 8);
  tinta_body_detail::write(bytes, 32, entry.record, 4);
  tinta_body_detail::write(bytes, 36, binary_record::crc32(bytes.data(), 36), 4);
  return true;
}
inline bool decodeJournalIdentityEntry(std::span<const uint8_t> bytes, JournalIdentityEntry& output) {
  if (bytes.size() != JOURNAL_IDENTITY_ENTRY_SIZE ||
      tinta_body_detail::read(bytes, 36, 4) != binary_record::crc32(bytes.data(), 36))
    return false;
  JournalIdentityEntry entry;
  std::copy_n(bytes.begin(), 16, entry.identity.origin.begin());
  entry.identity.epoch = tinta_body_detail::read(bytes, 16, 8);
  entry.identity.sequence = tinta_body_detail::read(bytes, 24, 8);
  entry.record = static_cast<uint32_t>(tinta_body_detail::read(bytes, 32, 4));
  if (!tinta_body_detail::validIdentity(entry.identity)) return false;
  output = entry;
  return true;
}
inline bool encodeJournalIdentityHeader(uint32_t count, uint32_t entriesCrc, std::span<uint8_t> bytes) {
  if (bytes.size() != JOURNAL_IDENTITY_HEADER_SIZE || count > UINT32_MAX / TintaJournal::RECORD_SIZE) return false;
  bytes[0] = 'J';
  bytes[1] = 'I';
  bytes[2] = 'X';
  bytes[3] = 1;
  tinta_body_detail::write(bytes, 4, count, 4);
  tinta_body_detail::write(bytes, 8, entriesCrc, 4);
  tinta_body_detail::write(bytes, 12, binary_record::crc32(bytes.data(), 12), 4);
  return true;
}
class JournalIdentityIndexStorage {
 public:
  virtual ~JournalIdentityIndexStorage() = default;
  virtual bool size(uint64_t& bytes) = 0;
  virtual bool read(uint64_t offset, std::span<uint8_t> bytes) = 0;
};
// Disposable index of one frozen journal. Scratch must be disjoint from journal scratch.
class IndexedJournalIdentities final : public JournalIdentityIndex {
 public:
  IndexedJournalIdentities(JournalIdentityIndexStorage& storage, std::span<uint8_t> scratch)
      : storage(storage), scratch(scratch) {}
  bool open(uint32_t expectedCount) {
    ready = false;
    uint64_t extent = 0;
    if (scratch.size() < JOURNAL_IDENTITY_ENTRY_SIZE || expectedCount > UINT32_MAX / TintaJournal::RECORD_SIZE ||
        !storage.size(extent) ||
        extent != JOURNAL_IDENTITY_HEADER_SIZE + uint64_t{expectedCount} * JOURNAL_IDENTITY_ENTRY_SIZE ||
        !storage.read(0, scratch.first(JOURNAL_IDENTITY_HEADER_SIZE)))
      return false;
    const auto header = scratch.first(JOURNAL_IDENTITY_HEADER_SIZE);
    if (header[0] != 'J' || header[1] != 'I' || header[2] != 'X' || header[3] != 1 ||
        tinta_body_detail::read(header, 4, 4) != expectedCount ||
        tinta_body_detail::read(header, 12, 4) != binary_record::crc32(header.data(), 12))
      return false;
    const auto expectedCrc = static_cast<uint32_t>(tinta_body_detail::read(header, 8, 4));
    uint32_t crc = 0;
    EventIdentity previous{};
    for (uint32_t at = 0; at < expectedCount; ++at) {
      if (!load(at) || entry.record >= expectedCount || (at && !journalIdentityBefore(previous, entry.identity)))
        return false;
      previous = entry.identity;
      crc = binary_record::crc32Update(crc, scratch.data(), JOURNAL_IDENTITY_ENTRY_SIZE);
    }
    if (crc != expectedCrc) return false;
    count = expectedCount;
    ready = true;
    return true;
  }
  bool available() const { return ready; }
  uint32_t recordCount() const { return ready ? count : 0; }
  bool read(uint32_t at, JournalIdentityEntry& output) {
    if (!ready || at >= count) return false;
    if (!load(at) || entry.record >= count) {
      ready = false;
      return false;
    }
    output = entry;
    return true;
  }
  JournalIdentityLookup find(const EventIdentity& identity, uint32_t& record) override {
    if (!ready || !tinta_body_detail::validIdentity(identity)) return JournalIdentityLookup::IoError;
    uint32_t low = 0, high = count;
    while (low < high) {
      const auto middle = low + (high - low) / 2;
      if (!load(middle) || entry.record >= count) {
        ready = false;
        return JournalIdentityLookup::IoError;
      }
      if (entry.identity == identity) {
        record = entry.record;
        return JournalIdentityLookup::Found;
      }
      if (journalIdentityBefore(entry.identity, identity))
        low = middle + 1;
      else
        high = middle;
    }
    return JournalIdentityLookup::Missing;
  }

 private:
  bool load(uint32_t at) {
    return storage.read(JOURNAL_IDENTITY_HEADER_SIZE + uint64_t{at} * JOURNAL_IDENTITY_ENTRY_SIZE,
                        scratch.first(JOURNAL_IDENTITY_ENTRY_SIZE)) &&
           decodeJournalIdentityEntry(scratch.first(JOURNAL_IDENTITY_ENTRY_SIZE), entry);
  }
  JournalIdentityIndexStorage& storage;
  std::span<uint8_t> scratch;
  JournalIdentityEntry entry{};
  uint32_t count = 0;
  bool ready = false;
};
}  // namespace companion

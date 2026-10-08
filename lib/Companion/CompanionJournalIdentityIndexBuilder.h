#pragma once

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionJournalIdentitySorter.h"

namespace companion {
class JournalIdentityIndexSink {
 public:
  virtual ~JournalIdentityIndexSink() = default;
  virtual bool begin() = 0;
  virtual bool write(uint64_t offset, std::span<const uint8_t> bytes) = 0;
  // Success durably publishes a complete index. Failed publication may require cache recovery.
  virtual bool finish(uint64_t bytes) = 0;
  virtual void abort() = 0;
};
class JournalIdentityIndexBuilder {
 public:
  JournalIdentityIndexBuilder(JournalIdentityIndexSink& sink, std::span<uint8_t> scratch)
      : sink(sink), scratch(scratch) {}
  bool build(JournalIdentitySource& sorted, uint32_t expectedCount) {
    if (scratch.size() < JOURNAL_IDENTITY_ENTRY_SIZE || expectedCount > UINT32_MAX / TintaJournal::RECORD_SIZE)
      return false;
    struct Guard {
      JournalIdentityIndexSink& sink;
      bool published = false;
      ~Guard() {
        if (!published) sink.abort();
      }
    } guard{sink};
    if (!sink.begin() || !header(expectedCount, 0)) return false;
    uint32_t count = 0, crc = 0;
    EventIdentity previous{};
    JournalIdentityEntry entry;
    for (;;) {
      const auto result = sorted.next(entry);
      if (result == JournalIdentitySourceResult::End) break;
      if (result != JournalIdentitySourceResult::Entry || count >= expectedCount || entry.record >= expectedCount ||
          (count && !journalIdentityBefore(previous, entry.identity)))
        return false;
      const auto bytes = scratch.first(JOURNAL_IDENTITY_ENTRY_SIZE);
      if (!encodeJournalIdentityEntry(entry, bytes) ||
          !sink.write(JOURNAL_IDENTITY_HEADER_SIZE + uint64_t{count} * JOURNAL_IDENTITY_ENTRY_SIZE, bytes))
        return false;
      crc = binary_record::crc32Update(crc, bytes.data(), bytes.size());
      previous = entry.identity;
      ++count;
    }
    if (count != expectedCount || !header(count, crc) ||
        !sink.finish(JOURNAL_IDENTITY_HEADER_SIZE + uint64_t{count} * JOURNAL_IDENTITY_ENTRY_SIZE))
      return false;
    guard.published = true;
    return true;
  }

 private:
  bool header(uint32_t count, uint32_t crc) {
    return encodeJournalIdentityHeader(count, crc, scratch.first(JOURNAL_IDENTITY_HEADER_SIZE)) &&
           sink.write(0, scratch.first(JOURNAL_IDENTITY_HEADER_SIZE));
  }
  JournalIdentityIndexSink& sink;
  std::span<uint8_t> scratch;
};
}  // namespace companion

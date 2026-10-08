#pragma once

#include <algorithm>
#include <cstring>

#include "CompanionJournalIdentityIndex.h"

namespace companion {
enum class JournalIdentitySourceResult { Entry, End, Error };
class JournalIdentitySource {
 public:
  virtual ~JournalIdentitySource() = default;
  virtual JournalIdentitySourceResult next(JournalIdentityEntry& entry) = 0;
};
class CommittedJournalIdentitySource final : public JournalIdentitySource {
 public:
  explicit CommittedJournalIdentitySource(TintaJournal& journal)
      : journal(journal), count(journal.count()), ready(journal.available()) {}
  JournalIdentitySourceResult next(JournalIdentityEntry& entry) override {
    if (!ready || !journal.available() || journal.count() != count) return JournalIdentitySourceResult::Error;
    if (cursor == count) return JournalIdentitySourceResult::End;
    if (journal.read(cursor) != TintaJournalResult::Ok) {
      ready = false;
      return JournalIdentitySourceResult::Error;
    }
    entry = {journal.event().identity, cursor++};
    return JournalIdentitySourceResult::Entry;
  }

 private:
  TintaJournal& journal;
  uint32_t count = 0, cursor = 0;
  bool ready = false;
};
// Two disposable runs; no run is authoritative until the index builder publishes it.
class JournalIdentitySortStorage {
 public:
  virtual ~JournalIdentitySortStorage() = default;
  virtual bool reset() = 0;
  virtual bool read(unsigned run, uint64_t offset, std::span<uint8_t> bytes) = 0;
  virtual bool write(unsigned run, uint64_t offset, std::span<const uint8_t> bytes) = 0;
  virtual bool finish(unsigned run, uint64_t size) = 0;
};
// Scratch remains borrowed through consumption and disjoint from journal scratch.
// Encoded records avoid alignment/lifetime casts; no heap allocation.
class JournalIdentitySorter final : public JournalIdentitySource {
 public:
  JournalIdentitySorter(JournalIdentitySortStorage& storage, std::span<uint8_t> scratch)
      : storage(storage), scratch(scratch) {}
  bool build(JournalIdentitySource& source) {
    ready = false;
    count = cursor = 0;
    active = 0;
    if (scratch.size() < 3 * JOURNAL_IDENTITY_ENTRY_SIZE || !storage.reset()) return false;
    const size_t capacity = scratch.size() / JOURNAL_IDENTITY_ENTRY_SIZE;
    size_t buffered = 0;
    JournalIdentityEntry entry;
    for (;;) {
      const auto result = source.next(entry);
      if (result == JournalIdentitySourceResult::Error) return false;
      if (result == JournalIdentitySourceResult::End) break;
      if (result != JournalIdentitySourceResult::Entry || count == UINT64_MAX / JOURNAL_IDENTITY_ENTRY_SIZE ||
          count == UINT32_MAX / TintaJournal::RECORD_SIZE ||
          !encodeJournalIdentityEntry(entry, record(scratch, buffered)))
        return false;
      ++count;
      if (++buffered == capacity) {
        if (!flushChunk(buffered)) return false;
        buffered = 0;
      }
    }
    if (buffered && !flushChunk(buffered)) return false;
    if (!storage.finish(0, count * JOURNAL_IDENTITY_ENTRY_SIZE)) return false;
    uint64_t width = capacity;
    while (width < count) {
      if (!mergePass(width)) return false;
      active ^= 1;
      width = width > count / 2 ? count : width * 2;
    }
    ready = true;
    return true;
  }
  JournalIdentitySourceResult next(JournalIdentityEntry& entry) override {
    if (!ready) return JournalIdentitySourceResult::Error;
    if (cursor == count) return JournalIdentitySourceResult::End;
    auto bytes = scratch.first(JOURNAL_IDENTITY_ENTRY_SIZE);
    if (!storage.read(active, cursor * JOURNAL_IDENTITY_ENTRY_SIZE, bytes) ||
        !decodeJournalIdentityEntry(bytes, entry)) {
      ready = false;
      return JournalIdentitySourceResult::Error;
    }
    ++cursor;
    return JournalIdentitySourceResult::Entry;
  }

 private:
  static std::span<uint8_t> record(std::span<uint8_t> bytes, size_t index) {
    return bytes.subspan(index * JOURNAL_IDENTITY_ENTRY_SIZE, JOURNAL_IDENTITY_ENTRY_SIZE);
  }
  static bool less(std::span<const uint8_t> left, std::span<const uint8_t> right) {
    if (!std::equal(left.begin(), left.begin() + 16, right.begin()))
      return std::lexicographical_compare(left.begin(), left.begin() + 16, right.begin(), right.begin() + 16);
    for (const size_t at : {size_t{16}, size_t{24}}) {
      const auto a = tinta_body_detail::read(left, at, 8), b = tinta_body_detail::read(right, at, 8);
      if (a != b) return a < b;
    }
    return false;
  }
  void swap(size_t left, size_t right) {
    auto first = record(scratch, left);
    auto second = record(scratch, right);
    std::swap_ranges(first.begin(), first.end(), second.begin());
  }
  void sift(size_t root, size_t end) {
    while (root < end / 2) {
      size_t child = root * 2 + 1;
      if (child + 1 < end && less(record(scratch, child), record(scratch, child + 1))) ++child;
      if (!less(record(scratch, root), record(scratch, child))) return;
      swap(root, child);
      root = child;
    }
  }
  bool flushChunk(size_t size) {
    for (size_t root = size / 2; root > 0;) sift(--root, size);
    for (size_t end = size; end > 1;) {
      swap(0, --end);
      sift(0, end);
    }
    return storage.write(0, (count - size) * JOURNAL_IDENTITY_ENTRY_SIZE,
                         scratch.first(size * JOURNAL_IDENTITY_ENTRY_SIZE));
  }
  struct Bank {
    std::span<uint8_t> bytes;
    uint64_t next = 0, end = 0;
    size_t index = 0, loaded = 0;
  };
  bool load(Bank& bank) {
    if (bank.index < bank.loaded || bank.next == bank.end) return true;
    bank.loaded =
        static_cast<size_t>(std::min<uint64_t>(bank.bytes.size() / JOURNAL_IDENTITY_ENTRY_SIZE, bank.end - bank.next));
    bank.index = 0;
    auto bytes = bank.bytes.first(bank.loaded * JOURNAL_IDENTITY_ENTRY_SIZE);
    if (!storage.read(active, bank.next * JOURNAL_IDENTITY_ENTRY_SIZE, bytes)) return false;
    JournalIdentityEntry entry;
    for (size_t index = 0; index < bank.loaded; ++index) {
      if (!decodeJournalIdentityEntry(record(bytes, index), entry)) return false;
    }
    bank.next += bank.loaded;
    return true;
  }
  static bool remaining(const Bank& bank) { return bank.index < bank.loaded || bank.next < bank.end; }
  bool mergePass(uint64_t width) {
    const size_t bankSize = scratch.size() / (3 * JOURNAL_IDENTITY_ENTRY_SIZE) * JOURNAL_IDENTITY_ENTRY_SIZE;
    auto output = scratch.subspan(2 * bankSize, bankSize);
    uint64_t written = 0;
    size_t buffered = 0;
    for (uint64_t start = 0; start < count;) {
      const uint64_t middle = start + std::min(width, count - start);
      const uint64_t end = middle + std::min(width, count - middle);
      Bank left{scratch.first(bankSize), start, middle};
      Bank right{scratch.subspan(bankSize, bankSize), middle, end};
      while (remaining(left) || remaining(right)) {
        if (!load(left) || !load(right)) return false;
        Bank& selected = !remaining(right) || (remaining(left) &&
                                               !less(record(right.bytes, right.index), record(left.bytes, left.index)))
                             ? left
                             : right;
        std::memcpy(record(output, buffered).data(), record(selected.bytes, selected.index++).data(),
                    JOURNAL_IDENTITY_ENTRY_SIZE);
        if (++buffered == output.size() / JOURNAL_IDENTITY_ENTRY_SIZE) {
          if (!storage.write(active ^ 1, written * JOURNAL_IDENTITY_ENTRY_SIZE, output)) return false;
          written += buffered;
          buffered = 0;
        }
      }
      start = end;
    }
    if (buffered && !storage.write(active ^ 1, written * JOURNAL_IDENTITY_ENTRY_SIZE,
                                   output.first(buffered * JOURNAL_IDENTITY_ENTRY_SIZE)))
      return false;
    return storage.finish(active ^ 1, count * JOURNAL_IDENTITY_ENTRY_SIZE);
  }
  JournalIdentitySortStorage& storage;
  std::span<uint8_t> scratch;
  uint64_t count = 0, cursor = 0;
  unsigned active = 0;
  bool ready = false;
};
}  // namespace companion

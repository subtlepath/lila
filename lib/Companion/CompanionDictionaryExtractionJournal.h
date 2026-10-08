#pragma once

#include "CompanionDictionaryExtractionReceipt.h"
#include "CompanionDictionaryJournalPaths.h"
#include "CompanionTransfer.h"

namespace companion {
enum class DictionaryJournalResult { Ok, Missing, Invalid, Conflict, Corrupt, IoError, Exhausted };
class DictionaryExtractionJournalStorage {
 public:
  virtual ~DictionaryExtractionJournalStorage() = default;
  virtual bool prepare() = 0;
  virtual FileStatus stat(const char* path, uint64_t& length) = 0;
  virtual bool read(const char* path, uint64_t at, std::span<uint8_t> bytes) = 0;
  // Successful writes must truncate and sync before returning.
  virtual bool write(const char* path, uint64_t at, std::span<const uint8_t> bytes, bool truncate) = 0;
};
// Session-owned redundant receipts. The parent exclusively owns the slots and
// stages; initial ownership must be persisted before creating any member stage.
class DictionaryExtractionJournal final {
 public:
  DictionaryExtractionJournal(DictionaryExtractionJournalStorage& storage, std::span<uint8_t> scratch)
      : storage(storage), scratch(scratch) {}
  const DictionaryExtractionReceipt* current() const { return ready ? &active : nullptr; }
  void invalidate() { ready = false; }
  DictionaryJournalResult recover(const Identity& transaction, const Identity& generation, const Digest& archiveHash) {
    ready = false;
    slot = -1;
    if (scratch.size() < DICTIONARY_EXTRACTION_RECEIPT_SIZE || !inventory_detail::nonzero(transaction) ||
        !inventory_detail::nonzero(generation) || !dictionaryReceiptHashPresent(archiveHash))
      return DictionaryJournalResult::Invalid;
    if (!storage.prepare()) return DictionaryJournalResult::IoError;
    bool present = false;
    for (const auto path : DICTIONARY_EXTRACTION_JOURNALS) {
      uint64_t length = 0;
      const auto status = storage.stat(path, length);
      if (status == FileStatus::Error) return DictionaryJournalResult::IoError;
      if (status == FileStatus::Missing) continue;
      present = true;
      if (length != DICTIONARY_EXTRACTION_RECEIPT_SIZE) continue;
      auto bytes = scratch.first(DICTIONARY_EXTRACTION_RECEIPT_SIZE);
      if (!storage.read(path, 0, bytes)) return DictionaryJournalResult::IoError;
      if (!decodeDictionaryExtractionReceipt(bytes, candidate)) continue;
      if (candidate.transaction != transaction || candidate.generation != generation ||
          candidate.archiveHash != archiveHash)
        return DictionaryJournalResult::Conflict;
      // recover() changes only matching parent fields in active, preserving this seed.
      active = candidate;
      return recover(active);
    }
    return present ? DictionaryJournalResult::Corrupt : DictionaryJournalResult::Missing;
  }
  DictionaryJournalResult recover(const DictionaryExtractionReceipt& expected) {
    ready = false;
    slot = -1;
    if (scratch.size() < DICTIONARY_EXTRACTION_RECEIPT_SIZE || !validDictionaryExtractionReceipt(expected))
      return DictionaryJournalResult::Invalid;
    if (!storage.prepare()) return DictionaryJournalResult::IoError;
    bool present = false;
    for (int at = 0; at < 2; ++at) {
      uint64_t length = 0;
      const auto status = storage.stat(DICTIONARY_EXTRACTION_JOURNALS[at], length);
      if (status == FileStatus::Error) return DictionaryJournalResult::IoError;
      if (status == FileStatus::Missing) continue;
      present = true;
      if (length != DICTIONARY_EXTRACTION_RECEIPT_SIZE) continue;
      auto bytes = scratch.first(DICTIONARY_EXTRACTION_RECEIPT_SIZE);
      if (!storage.read(DICTIONARY_EXTRACTION_JOURNALS[at], 0, bytes)) return DictionaryJournalResult::IoError;
      if (!decodeDictionaryExtractionReceipt(bytes, candidate)) continue;
      if (!sameParent(candidate, expected)) return DictionaryJournalResult::Conflict;
      if (slot >= 0) {
        if (candidate.revision == active.revision && candidate != active) return DictionaryJournalResult::Corrupt;
        if (candidate.revision > active.revision
                ? !successor(active, candidate)
                : candidate.revision < active.revision && !successor(candidate, active))
          return DictionaryJournalResult::Corrupt;
      }
      if (slot < 0 || candidate.revision > active.revision) {
        active = candidate;
        slot = at;
      }
    }
    if (slot < 0) return present ? DictionaryJournalResult::Corrupt : DictionaryJournalResult::Missing;
    ready = true;
    return DictionaryJournalResult::Ok;
  }
  DictionaryJournalResult begin(const DictionaryExtractionReceipt& initial) {
    if (initial.revision != 1 || initial.sealed != 0) return DictionaryJournalResult::Invalid;
    const auto result = recover(initial);
    if (result != DictionaryJournalResult::Missing) {
      ready = false;
      return result == DictionaryJournalResult::Ok ? DictionaryJournalResult::Conflict : result;
    }
    candidate = initial;
    return persist();
  }
  DictionaryJournalResult recordSealed(unsigned member, const Digest& hash) {
    if (!ready || member >= 4 || !dictionaryReceiptHashPresent(hash)) return DictionaryJournalResult::Invalid;
    if (active.sealed & (1u << member))
      return active.hashes[member] == hash ? DictionaryJournalResult::Ok : DictionaryJournalResult::Conflict;
    if (active.revision == UINT64_MAX) return DictionaryJournalResult::Exhausted;
    candidate = active;
    ++candidate.revision;
    candidate.sealed |= 1u << member;
    candidate.hashes[member] = hash;
    if (!validDictionaryExtractionReceipt(candidate)) return DictionaryJournalResult::Invalid;
    return persist();
  }

 private:
  DictionaryExtractionJournalStorage& storage;
  std::span<uint8_t> scratch;
  DictionaryExtractionReceipt active, candidate, readback;
  int slot = -1;
  bool ready = false;
  static bool sameParent(const DictionaryExtractionReceipt& a, const DictionaryExtractionReceipt& b) {
    return a.transaction == b.transaction && a.generation == b.generation && a.archiveHash == b.archiveHash &&
           a.lengths == b.lengths && a.compressed == b.compressed && a.synonyms == b.synonyms;
  }
  static bool successor(const DictionaryExtractionReceipt& previous, const DictionaryExtractionReceipt& next) {
    if (previous.revision == UINT64_MAX || next.revision != previous.revision + 1 ||
        (previous.sealed & next.sealed) != previous.sealed || previous.sealed == next.sealed)
      return false;
    const unsigned added = next.sealed ^ previous.sealed;
    if (added & (added - 1)) return false;
    for (unsigned at = 0; at < 4; ++at)
      if ((previous.sealed & (1u << at)) && previous.hashes[at] != next.hashes[at]) return false;
    return true;
  }
  DictionaryJournalResult persist() {
    ready = false;
    auto bytes = scratch.first(DICTIONARY_EXTRACTION_RECEIPT_SIZE);
    if (!encodeDictionaryExtractionReceipt(candidate, bytes)) return DictionaryJournalResult::Invalid;
    const int nextSlot = slot == 0 ? 1 : 0;
    const auto path = DICTIONARY_EXTRACTION_JOURNALS[nextSlot];
    if (!storage.write(path, 0, bytes, true)) return DictionaryJournalResult::IoError;
    uint64_t length = 0;
    if (storage.stat(path, length) != FileStatus::Present || length != bytes.size() || !storage.read(path, 0, bytes))
      return DictionaryJournalResult::IoError;
    if (!decodeDictionaryExtractionReceipt(bytes, readback) || readback != candidate)
      return DictionaryJournalResult::Corrupt;
    active = candidate;
    slot = nextSlot;
    ready = true;
    return DictionaryJournalResult::Ok;
  }
};
}  // namespace companion

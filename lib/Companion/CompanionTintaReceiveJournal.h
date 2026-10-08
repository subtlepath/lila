#pragma once

#include "CompanionTintaReceiveCheckpoint.h"

namespace companion {
class TintaReceiveJournalStorage {
 public:
  virtual ~TintaReceiveJournalStorage() = default;
  // Missing slots return length zero. Successful writes must be synced.
  virtual bool read(uint8_t slot, std::span<uint8_t> bytes, size_t& length) = 0;
  virtual bool write(uint8_t slot, std::span<const uint8_t> bytes) = 0;
};
enum class TintaReceiveJournalResult { Ok, Missing, Invalid, Corrupt, IoError, Unavailable };
// Session-owned; caller scratch and storage must outlive it. No heap allocation.
class TintaReceiveJournal {
 public:
  TintaReceiveJournal(TintaReceiveJournalStorage& storage, std::span<uint8_t> scratch)
      : storage(storage), scratch(scratch) {}
  TintaReceiveJournalResult open() {
    ready = empty = false;
    if (scratch.size() < TINTA_RECEIVE_CHECKPOINT_SIZE) return TintaReceiveJournalResult::Unavailable;
    bool found = false, present = false;
    for (uint8_t slot = 0; slot < 2; ++slot) {
      size_t length = 0;
      if (!storage.read(slot, scratch.first(TINTA_RECEIVE_CHECKPOINT_SIZE), length))
        return TintaReceiveJournalResult::IoError;
      present |= length != 0;
      if (length != TINTA_RECEIVE_CHECKPOINT_SIZE || !decodeTintaReceiveCheckpoint(scratch.first(length), candidate) ||
          (candidate.sequence & 1) != slot)
        continue;
      if (found) {
        const auto& newer = candidate.sequence > current.sequence ? candidate : current;
        const auto& older = candidate.sequence > current.sequence ? current : candidate;
        if (!follows(older, newer)) return TintaReceiveJournalResult::Corrupt;
      }
      if (!found || candidate.sequence > current.sequence) current = candidate;
      found = true;
    }
    if (!found) {
      empty = !present;
      return present ? TintaReceiveJournalResult::Corrupt : TintaReceiveJournalResult::Missing;
    }
    ready = true;
    return TintaReceiveJournalResult::Ok;
  }
  const TintaReceiveCheckpoint* checkpoint() const { return ready ? &current : nullptr; }
  TintaReceiveJournalResult start(const TintaReceiveCheckpoint& value) {
    if (!empty) return TintaReceiveJournalResult::Unavailable;
    if (!validTintaReceiveCheckpoint(value) || value.sequence != 1 || value.offset || value.sealedMask ||
        value.file != TintaDerivedFile::Items)
      return TintaReceiveJournalResult::Invalid;
    return persist(value);
  }
  // Caller syncs candidate bytes before advancing offset, and verifies SHA before switching files.
  TintaReceiveJournalResult advance(const TintaReceiveCheckpoint& value) {
    if (!ready) return TintaReceiveJournalResult::Unavailable;
    if (value == current) return TintaReceiveJournalResult::Ok;
    if (!validTintaReceiveCheckpoint(value) || !follows(current, value)) return TintaReceiveJournalResult::Invalid;
    return persist(value);
  }

 private:
  static bool follows(const TintaReceiveCheckpoint& old, const TintaReceiveCheckpoint& next) {
    if (old.sequence == UINT64_MAX || next.sequence != old.sequence + 1 || old.course != next.course ||
        old.transaction != next.transaction || old.owner != next.owner || old.storage != next.storage ||
        old.manifestHash != next.manifestHash)
      return false;
    if (old.file == next.file)
      return next.sealedMask == old.sealedMask && next.length == old.length && next.offset >= old.offset;
    const auto index = static_cast<unsigned>(old.file);
    return index < 4 && static_cast<unsigned>(next.file) == index + 1 && old.offset == old.length && next.offset == 0 &&
           next.sealedMask == static_cast<uint8_t>(old.sealedMask | (1u << index));
  }
  TintaReceiveJournalResult persist(const TintaReceiveCheckpoint& value) {
    ready = empty = false;
    if (!encodeTintaReceiveCheckpoint(value, scratch.first(TINTA_RECEIVE_CHECKPOINT_SIZE)))
      return TintaReceiveJournalResult::Invalid;
    const auto slot = static_cast<uint8_t>(value.sequence & 1);
    if (!storage.write(slot, scratch.first(TINTA_RECEIVE_CHECKPOINT_SIZE))) return TintaReceiveJournalResult::IoError;
    size_t length = 0;
    if (!storage.read(slot, scratch.first(TINTA_RECEIVE_CHECKPOINT_SIZE), length))
      return TintaReceiveJournalResult::IoError;
    if (length != TINTA_RECEIVE_CHECKPOINT_SIZE || !decodeTintaReceiveCheckpoint(scratch.first(length), candidate) ||
        candidate != value)
      return TintaReceiveJournalResult::Corrupt;
    current = candidate;
    ready = true;
    return TintaReceiveJournalResult::Ok;
  }
  TintaReceiveJournalStorage& storage;
  std::span<uint8_t> scratch;
  TintaReceiveCheckpoint current{}, candidate{};
  bool ready = false, empty = false;
};
}  // namespace companion

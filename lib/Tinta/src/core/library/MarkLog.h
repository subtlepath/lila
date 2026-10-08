#pragma once

#include <cstdint>

#include "core/StateStore.h"

namespace tinta::core::library {

// A small set of 32-bit keys kept on the card (PLAN.md M6): the words starred
// for the next session (item uids) and the readings finished (story keys).
//
// The file is a 4-byte magic "TMK1" and 8-byte records, appended:
//
//   0 key u32  4 op u8 (1 add, 2 remove)  5 zero u8  6 check u16
//
// check is the low half of the CRC-32 of bytes 0..5. open() replays them in
// order; a record that fails its check (a torn append) is skipped, so a cut
// loses at most the change being written. Once the file holds many more
// records than keys it is rewritten with one add per key (replace(), which is
// atomic).
//
// Without a card (a guest) the set lives in RAM for this power-on only.
class MarkLog {
 public:
  static constexpr uint16_t kCapacity = 96;
  static constexpr uint32_t kRecordSize = 8;
  static constexpr uint32_t kHeaderSize = 4;

  MarkLog(StateStore& store, const char* file) : store_(store), file_(file) {}

  // Reads the file into RAM; an absent or unreadable file is an empty set.
  void open();

  struct MutationJournal {
    void* context = nullptr;
    bool (*persist)(void*, uint32_t key, bool enabled) = nullptr;
    bool (*recover)(void*) = nullptr;
  };
  // The owner logs failures and outlives this log and every callback.
  void setMutationJournal(MutationJournal journal) { mutationJournal_ = journal; }
  bool journalFailed() const { return journalFailed_; }

  bool contains(uint32_t key) const;
  // False when the set is full or the record could not be written (it is
  // then kept in RAM regardless, for this power-on).
  bool add(uint32_t key);
  bool remove(uint32_t key);

  uint16_t count() const { return count_; }
  uint32_t at(uint16_t i) const { return i < count_ ? keys_[i] : 0; }

 private:
  bool persistMutation(uint32_t key, bool enabled);
  bool append(uint32_t key, uint8_t op);
  void compact();
  void insert(uint32_t key);
  void erase(uint32_t key);

  MutationJournal mutationJournal_;
  bool journalFailed_ = false;
  StateStore& store_;
  const char* file_;
  uint32_t keys_[kCapacity] = {};
  uint16_t count_ = 0;
  uint32_t records_ = 0;  // records in the file
};

}  // namespace tinta::core::library

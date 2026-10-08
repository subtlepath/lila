#include "core/srs/ProgressStore.h"

#include <cstring>

#include "core/srs/Bytes.h"

namespace tinta::core {

namespace {

constexpr uint8_t kMagic[4] = {'T', 'I', 'S', '1'};
constexpr uint16_t kVersion = 1;
constexpr uint16_t kHeaderPending = 1u << 0;
constexpr uint16_t kHeaderUndo = 1u << 1;
constexpr uint32_t kJournalChunk = 4;

uint16_t saturatingIncrement(uint16_t v) { return v == 0xFFFF ? v : static_cast<uint16_t>(v + 1); }

}  // namespace

// --- JournalEntry -----------------------------------------------------------

JournalEntry JournalEntry::review(uint32_t uid, Grade grade, uint8_t format, uint32_t responseMs, DayNumber day,
                                  uint32_t time) {
  JournalEntry e;
  e.uid = uid;
  e.time = time;
  e.day = day;
  e.op = static_cast<uint8_t>((static_cast<uint8_t>(grade) & 0x07) | ((format & 0x1F) << 3));
  uint32_t quarters = responseMs / 250 + (responseMs % 250 >= 125 ? 1 : 0);
  if (quarters > 255) quarters = 255;
  if (quarters == 0 && responseMs > 0) quarters = 1;
  e.arg = static_cast<uint8_t>(quarters);
  return e;
}

JournalEntry JournalEntry::control(uint32_t uid, uint8_t code, uint8_t arg, DayNumber day, uint32_t time) {
  JournalEntry e;
  e.uid = uid;
  e.time = time;
  e.day = day;
  e.op = static_cast<uint8_t>((code & 0x1F) << 3);
  e.arg = arg;
  return e;
}

void JournalEntry::encode(uint8_t out[kSize]) const {
  putU32(out + 0, uid);
  putU32(out + 4, time);
  putU16(out + 8, day);
  out[10] = op;
  out[11] = arg;
}

JournalEntry JournalEntry::decode(const uint8_t in[kSize]) {
  JournalEntry e;
  e.uid = getU32(in + 0);
  e.time = getU32(in + 4);
  e.day = getU16(in + 8);
  e.op = in[10];
  e.arg = in[11];
  return e;
}

// --- Header -----------------------------------------------------------------
//
//   0 magic "TIS1"     4 version u16      6 size u16       8 seq u32
//  12 recordCount u32 16 journalCount u32
//  20 statDay u16     22 statNew u16     24 statReviews u16
//  26 flags u16 (pending, undo)
//  28 pendingSlot u32 32 pending record (16)
//  48 undoSlot u32    52 undo before-image (16)
//  68 undoStatDay u16 70 undoStatNew u16 72 undoStatReviews u16  74 zero u16
//  76 crc32 of bytes 0..75

void ProgressStore::encodeHeader(const Header& h, uint8_t out[kHeaderSize]) {
  std::memset(out, 0, kHeaderSize);
  std::memcpy(out, kMagic, 4);
  putU16(out + 4, kVersion);
  putU16(out + 6, static_cast<uint16_t>(kHeaderSize));
  putU32(out + 8, h.seq);
  putU32(out + 12, h.recordCount);
  putU32(out + 16, h.journalCount);
  putU16(out + 20, h.statDay);
  putU16(out + 22, h.statNew);
  putU16(out + 24, h.statReviews);
  putU16(out + 26, static_cast<uint16_t>((h.pendingValid ? kHeaderPending : 0) | (h.undoValid ? kHeaderUndo : 0)));
  putU32(out + 28, h.pendingSlot);
  if (h.pendingValid) h.pending.encode(out + 32);
  putU32(out + 48, h.undoSlot);
  if (h.undoValid) h.undoBefore.encode(out + 52);
  putU16(out + 68, h.undoStatDay);
  putU16(out + 70, h.undoStatNew);
  putU16(out + 72, h.undoStatReviews);
  putU32(out + 76, crc32(out, 76));
}

bool ProgressStore::decodeHeader(const uint8_t in[kHeaderSize], Header& out) {
  if (std::memcmp(in, kMagic, 4) != 0) return false;
  if (getU16(in + 4) != kVersion || getU16(in + 6) != kHeaderSize) return false;
  if (getU32(in + 76) != crc32(in, 76)) return false;
  Header h;
  h.seq = getU32(in + 8);
  h.recordCount = getU32(in + 12);
  h.journalCount = getU32(in + 16);
  h.statDay = getU16(in + 20);
  h.statNew = getU16(in + 22);
  h.statReviews = getU16(in + 24);
  const uint16_t flags = getU16(in + 26);
  h.pendingValid = (flags & kHeaderPending) != 0;
  h.undoValid = (flags & kHeaderUndo) != 0;
  h.pendingSlot = getU32(in + 28);
  if (h.pendingValid && !ItemState::decode(in + 32, h.pending)) return false;
  h.undoSlot = getU32(in + 48);
  if (h.undoValid && !ItemState::decode(in + 52, h.undoBefore)) return false;
  h.undoStatDay = getU16(in + 68);
  h.undoStatNew = getU16(in + 70);
  h.undoStatReviews = getU16(in + 72);
  if (h.recordCount > kMaxRecords) return false;
  if (h.pendingValid && h.pendingSlot >= h.recordCount) return false;
  if (h.undoValid && h.undoSlot >= h.recordCount) return false;
  out = h;
  return true;
}

// --- Open, rebuild ----------------------------------------------------------

ProgressStore::ProgressStore(StateStore& store, const ItemCatalog& catalog, const Fsrs& fsrs)
    : store_(store), catalog_(catalog), fsrs_(fsrs) {}

void ProgressStore::clearSlots() {
  for (uint32_t i = 0; i < slotCount_; ++i) slots_[i] = kNoSlot;
}

ProgressStore::OpenResult ProgressStore::fail() {
  rebuilding_ = false;
  mode_ = Mode::Failed;
  return OpenResult::Failed;
}

ProgressStore::OpenResult ProgressStore::open(uint16_t* slots, uint32_t slotCount, ItemState* guestRecords,
                                              uint16_t guestCapacity) {
  mode_ = Mode::Closed;
  authoritativeRecoveryFailed_ = false;
  header_ = Header();
  slots_ = slots;
  slotCount_ = slots ? slotCount : 0;
  guest_ = guestRecords;
  guestCapacity_ = guestRecords ? guestCapacity : 0;
  if (slots_ == nullptr || slotCount_ < catalog_.itemCount()) return fail();
  clearSlots();

  if (!store_.available()) {
    mode_ = Mode::Guest;
    return OpenResult::Guest;
  }
  if (mutationJournal_.recover && !mutationJournal_.recover(mutationJournal_.context)) {
    authoritativeRecoveryFailed_ = true;
    return fail();
  }
  mode_ = Mode::Disk;

  Header h;
  if (store_.size(kItemsFile) < 0 || !loadHeader(h)) {
    if (journalRecords() == 0) {
      if (store_.size(kItemsFile) < 0) return createEmpty() ? OpenResult::Created : fail();
      // No header and no log to rebuild from: keep whatever reads back.
      const uint32_t whole = wholeRecords();
      header_.recordCount = whole < kMaxRecords ? whole : kMaxRecords;
      return salvage(0, true);
    }
    return rebuild();
  }
  header_ = h;

  // The header is written before the record it names: write it again,
  // unless the file has lost more than that record (a gap would read back
  // as records).
  if (h.pendingValid && h.pendingSlot <= wholeRecords() && !writeRecord(h.pendingSlot, h.pending)) {
    return fail();
  }

  // Rebuilding is safe only if the log holds everything items.bin reflects.
  const uint32_t journalCount = journalRecords();
  if (journalCount < h.journalCount) {
    const uint32_t whole = wholeRecords();
    const bool cut = whole < h.recordCount;
    if (cut) header_.recordCount = whole;
    return salvage(journalCount, cut);
  }
  if (wholeRecords() < h.recordCount) return rebuild();
  switch (mapRecords(false)) {
    case Map::Ok:
    case Map::Salvaged:
      break;
    case Map::Corrupt:
      return rebuild();
    case Map::IoError:
      return fail();
  }
  bool replayed = false;
  if (!replayTail(replayed)) return fail();
  return replayed ? OpenResult::Replayed : OpenResult::Opened;
}

// Keeps what items.bin holds, dropping damaged records, and commits it at
// once with the count continuing from the log as it is, so that the next
// open finds a consistent pair of files.
ProgressStore::OpenResult ProgressStore::salvage(uint32_t journalCount, bool damaged) {
  if (!store_.available()) return fail();  // see rebuild()
  switch (mapRecords(true)) {
    case Map::Ok:
      break;
    case Map::Salvaged:
    case Map::Corrupt:
      damaged = true;
      break;
    case Map::IoError:
      return fail();
  }
  header_.journalCount = journalCount;
  header_.pendingValid = false;
  header_.undoValid = false;
  if (!writeHeader(header_)) return fail();
  return damaged ? OpenResult::Salvaged : OpenResult::Opened;
}

uint32_t ProgressStore::wholeRecords() {
  const int32_t size = store_.size(kItemsFile);
  if (size < static_cast<int32_t>(kRecordsOffset)) return 0;
  return (static_cast<uint32_t>(size) - kRecordsOffset) / ItemState::kPackedSize;
}

uint32_t ProgressStore::journalRecords() {
  const int32_t size = store_.size(kJournalFile);
  return size > 0 ? static_cast<uint32_t>(size) / JournalEntry::kSize : 0;
}

ProgressStore::OpenResult ProgressStore::rebuild() {
  if (mode_ == Mode::Guest) return OpenResult::Guest;
  // StateStore::size() answers -1 for a missing file and for an I/O error
  // alike, and the platform store reports itself unavailable once the card
  // stops answering. Whatever made this look necessary, a card that has gone
  // away must not have its files replaced.
  if (slots_ == nullptr) return fail();
  const uint32_t count = journalRecords();
  if (!store_.available()) return fail();  // that -1 was the card going
  mode_ = Mode::Disk;
  clearSlots();
  header_ = Header();

  // Blank headers first: a rebuild cut short leaves no valid header, so the
  // next open starts it again rather than trusting half a file.
  store_.remove(kItemsFile);
  if (!zeroRange(kItemsFile, 0, kRecordsOffset, false)) return fail();

  rebuilding_ = true;
  uint32_t end = 0;  // past the last record that applied; a garbage tail is dropped
  uint8_t buffer[kJournalChunk * JournalEntry::kSize];
  for (uint32_t first = 0; first < count; first += kJournalChunk) {
    uint32_t n = count - first;
    if (n > kJournalChunk) n = kJournalChunk;
    const uint32_t bytes = n * JournalEntry::kSize;
    if (store_.read(kJournalFile, first * JournalEntry::kSize, buffer, bytes) != static_cast<int32_t>(bytes)) {
      return fail();
    }
    for (uint32_t i = 0; i < n; ++i) {
      const JournalEntry entry = JournalEntry::decode(buffer + i * JournalEntry::kSize);
      Change change;
      const Plan plan = this->plan(entry, true, change);
      if (plan == Plan::IoError) return fail();
      if (plan == Plan::Ok) {
        if (!commit(change)) return fail();
        end = first + i + 1;
      } else {
        // A garbage or unusable record: kept in the log as a no-op.
        header_.journalCount += 1;
      }
    }
  }
  rebuilding_ = false;

  header_.journalCount = end;
  header_.pendingValid = false;
  if (!writeHeader(header_)) return fail();
  return OpenResult::Rebuilt;
}

bool ProgressStore::createEmpty() {
  if (!store_.available()) return false;  // see rebuild()
  header_ = Header();
  if (!zeroRange(kItemsFile, 0, kRecordsOffset, false)) return false;
  Header h;
  if (!writeHeader(h)) return false;
  header_ = h;
  return true;
}

bool ProgressStore::loadHeader(Header& out) {
  uint8_t buffer[kHeaderSize];
  Header b;
  const bool haveA =
      store_.read(kItemsFile, 0, buffer, kHeaderSize) == static_cast<int32_t>(kHeaderSize) && decodeHeader(buffer, out);
  const bool haveB =
      store_.read(kItemsFile, kHeaderOffsetB, buffer, kHeaderSize) == static_cast<int32_t>(kHeaderSize) &&
      decodeHeader(buffer, b);
  if (haveB && (!haveA || b.seq > out.seq)) out = b;
  return haveA || haveB;
}

bool ProgressStore::writeHeader(Header& h) {
  h.seq = header_.seq + 1;  // h may be header_ itself
  uint8_t buffer[kHeaderSize];
  encodeHeader(h, buffer);
  return store_.write(kItemsFile, (h.seq & 1) ? kHeaderOffsetB : 0, buffer, kHeaderSize);
}

// With `salvage`, damaged records and repeated uids are overwritten with a
// dead record instead of failing the whole file.
ProgressStore::Map ProgressStore::mapRecords(bool salvage) {
  clearSlots();
  bool skipped = false;
  uint8_t buffer[kChunkRecords * ItemState::kPackedSize];
  for (uint32_t first = 0; first < header_.recordCount; first += kChunkRecords) {
    uint32_t n = header_.recordCount - first;
    if (n > kChunkRecords) n = kChunkRecords;
    if (!readRecords(first, n, buffer)) return Map::IoError;
    for (uint32_t i = 0; i < n; ++i) {
      ItemState state;
      int32_t index = -1;
      bool bad = !ItemState::decode(buffer + i * ItemState::kPackedSize, state);
      if (!bad) {
        index = catalog_.indexOfUid(state.uid);
        // A uid twice is not a file this code wrote.
        bad = index >= 0 && slots_[index] != kNoSlot;
      }
      if (bad) {
        if (!salvage) return Map::Corrupt;
        if (!writeRecord(first + i, ItemState::fresh(kDeadUid))) return Map::IoError;
        skipped = true;
        continue;
      }
      if (index < 0) continue;  // retired (or dead): kept on disk, not mapped
      slots_[index] = static_cast<uint16_t>((first + i) | (state.isNew() ? kUnreviewedBit : 0));
    }
  }
  return skipped ? Map::Salvaged : Map::Ok;
}

bool ProgressStore::replayTail(bool& replayed) {
  const uint32_t count = journalRecords();
  for (uint32_t k = header_.journalCount; k < count; ++k) {
    uint8_t buffer[JournalEntry::kSize];
    if (store_.read(kJournalFile, k * JournalEntry::kSize, buffer, JournalEntry::kSize) !=
        static_cast<int32_t>(JournalEntry::kSize)) {
      return false;
    }
    const JournalEntry entry = JournalEntry::decode(buffer);
    Change change;
    const Plan plan = this->plan(entry, false, change);
    if (plan == Plan::IoError) return false;
    if (plan != Plan::Ok) {
      // A torn append. Blank it and anything after it so that a later,
      // shorter header can never replay it.
      return zeroRange(kJournalFile, k * JournalEntry::kSize, count * JournalEntry::kSize, true);
    }
    if (!commit(change)) return false;
    replayed = true;
  }
  return true;
}

bool ProgressStore::zeroRange(const char* name, uint32_t from, uint32_t to, bool onlyIfDirty) {
  uint8_t buffer[64];
  for (uint32_t at = from; at < to; at += sizeof buffer) {
    uint32_t n = to - at;
    if (n > sizeof buffer) n = sizeof buffer;
    if (onlyIfDirty) {
      if (store_.read(name, at, buffer, n) != static_cast<int32_t>(n)) return false;
      bool dirty = false;
      for (uint32_t i = 0; i < n; ++i) dirty = dirty || buffer[i] != 0;
      if (!dirty) continue;
    }
    std::memset(buffer, 0, sizeof buffer);
    if (!store_.write(name, at, buffer, n)) return false;
  }
  return true;
}

// --- Records ----------------------------------------------------------------

bool ProgressStore::readRecords(uint32_t first, uint32_t count, uint8_t* buffer) {
  const uint32_t bytes = count * ItemState::kPackedSize;
  if (store_.read(kItemsFile, kRecordsOffset + first * ItemState::kPackedSize, buffer, bytes) ==
      static_cast<int32_t>(bytes)) {
    return true;
  }
  // The card is gone or the file is short: refuse changes until reopened.
  mode_ = Mode::Failed;
  return false;
}

bool ProgressStore::readRecord(uint32_t slot, ItemState& out) {
  if (mode_ == Mode::Guest) {
    if (slot >= header_.recordCount) return false;
    out = guest_[slot];
    return true;
  }
  uint8_t buffer[ItemState::kPackedSize];
  return readRecords(slot, 1, buffer) && ItemState::decode(buffer, out);
}

bool ProgressStore::writeRecord(uint32_t slot, const ItemState& state) {
  uint8_t buffer[ItemState::kPackedSize];
  state.encode(buffer);
  return store_.write(kItemsFile, kRecordsOffset + slot * ItemState::kPackedSize, buffer, ItemState::kPackedSize);
}

bool ProgressStore::findRecord(uint32_t uid, uint32_t& slot) {
  slot = kMaxRecords;
  if (mode_ != Mode::Disk) return true;
  uint8_t buffer[kChunkRecords * ItemState::kPackedSize];
  for (uint32_t first = 0; first < header_.recordCount; first += kChunkRecords) {
    uint32_t n = header_.recordCount - first;
    if (n > kChunkRecords) n = kChunkRecords;
    if (!readRecords(first, n, buffer)) return false;
    for (uint32_t i = 0; i < n; ++i) {
      if (getU32(buffer + i * ItemState::kPackedSize) == uid) {
        slot = first + i;
        return true;
      }
    }
  }
  return true;
}

bool ProgressStore::resolveSlot(uint32_t uid, int32_t& index, uint32_t& slot, bool& exists) {
  index = catalog_.indexOfUid(uid);
  if (index >= 0) {
    const uint16_t s = slots_[index];
    exists = s != kNoSlot;
    slot = s & static_cast<uint16_t>(~kUnreviewedBit);
    return true;
  }
  // Not in this catalog: a retired item, found by a scan (rare).
  if (!findRecord(uid, slot)) return false;
  exists = slot != kMaxRecords;
  return true;
}

// --- Changes ----------------------------------------------------------------

ProgressStore::Plan ProgressStore::plan(const JournalEntry& entry, bool allowUnknownUid, Change& out) {
  out.next = header_;
  out.next.journalCount = header_.journalCount + 1;

  const uint8_t code = entry.controlCode();
  if (entry.isReview() || code == JournalEntry::kSetFlags) {
    bool exists = false;
    if (!resolveSlot(entry.uid, out.index, out.slot, exists)) return Plan::IoError;
    if (exists) {
      if (!readRecord(out.slot, out.before)) return Plan::IoError;
    } else {
      if (out.index < 0 && !allowUnknownUid) return Plan::Invalid;
      const uint32_t capacity = mode_ == Mode::Guest ? guestCapacity_ : kMaxRecords;
      if (header_.recordCount >= capacity) return Plan::NoRoom;
      out.slot = header_.recordCount;
      out.next.recordCount = header_.recordCount + 1;
      out.before = ItemState::fresh(entry.uid);
    }
    out.after = out.before;

    if (entry.isReview()) {
      out.outcome = applyReview(fsrs_, out.after, entry.grade(), entry.day);
      out.next.undoValid = true;
      out.next.undoSlot = out.slot;
      out.next.undoBefore = out.before;
      out.next.undoStatDay = header_.statDay;
      out.next.undoStatNew = header_.statNew;
      out.next.undoStatReviews = header_.statReviews;
      if (out.next.statDay != entry.day) {
        out.next.statDay = entry.day;
        out.next.statNew = 0;
        out.next.statReviews = 0;
      }
      if (out.before.isNew()) {
        out.next.statNew = saturatingIncrement(out.next.statNew);
      } else if (out.before.lastDay != entry.day) {
        out.next.statReviews = saturatingIncrement(out.next.statReviews);
      }
    } else {
      out.after.flags = entry.arg & item_flag::kAll;
      out.next.undoValid = false;
    }
  } else if (code == JournalEntry::kUndo) {
    if (!header_.undoValid || header_.undoBefore.uid != entry.uid) return Plan::Invalid;
    out.slot = header_.undoSlot;
    if (!readRecord(out.slot, out.before)) return Plan::IoError;
    out.after = header_.undoBefore;
    out.index = catalog_.indexOfUid(entry.uid);
    out.next.statDay = header_.undoStatDay;
    out.next.statNew = header_.undoStatNew;
    out.next.statReviews = header_.undoStatReviews;
    out.next.undoValid = false;
  } else {
    return Plan::Invalid;
  }

  out.next.pendingValid = true;
  out.next.pendingSlot = out.slot;
  out.next.pending = out.after;
  return Plan::Ok;
}

bool ProgressStore::commit(const Change& change) {
  if (mode_ == Mode::Guest) {
    guest_[change.slot] = change.after;
    header_ = change.next;
  } else if (rebuilding_) {
    if (!writeRecord(change.slot, change.after)) return false;
    header_ = change.next;
  } else {
    Header h = change.next;
    if (!writeHeader(h)) return false;
    header_ = h;
    if (!writeRecord(change.slot, change.after)) return false;
  }
  if (change.index >= 0) {
    slots_[change.index] = static_cast<uint16_t>(change.slot | (change.after.isNew() ? kUnreviewedBit : 0));
  }
  return true;
}

ProgressStore::Status ProgressStore::apply(const JournalEntry& entry, Change& change, uint32_t responseMilliseconds) {
  if (mode_ != Mode::Disk && mode_ != Mode::Guest) return Status::Failed;
  const Plan plan = this->plan(entry, false, change);
  if (plan == Plan::IoError) {
    mode_ = Mode::Failed;
    return Status::Failed;
  }
  if (plan == Plan::Invalid) return Status::Invalid;
  if (plan == Plan::NoRoom) return Status::NotStored;
  if (mode_ == Mode::Disk && mutationJournal_.persist &&
      !mutationJournal_.persist(mutationJournal_.context, entry, change.before, change.after, responseMilliseconds)) {
    mode_ = Mode::Failed;
    return Status::Failed;
  }
  if (mode_ == Mode::Disk) {
    uint8_t buffer[JournalEntry::kSize];
    entry.encode(buffer);
    if (!store_.write(kJournalFile, header_.journalCount * JournalEntry::kSize, buffer, JournalEntry::kSize)) {
      mode_ = Mode::Failed;
      return Status::Failed;
    }
  }
  if (!commit(change)) {
    mode_ = Mode::Failed;
    return Status::Failed;
  }
  if (mode_ == Mode::Disk && mutationJournal_.committed &&
      !mutationJournal_.committed(mutationJournal_.context, entry, change.before, change.after, responseMilliseconds)) {
    mode_ = Mode::Failed;
    return Status::Failed;
  }
  return Status::Stored;
}

// --- Public operations ------------------------------------------------------

bool ProgressStore::verifyCommittedMutation(const JournalEntry& entry, const ItemState& before,
                                            const ItemState& after) {
  if (mode_ != Mode::Disk || !store_.available() || entry.uid != before.uid || entry.uid != after.uid) return false;
  Header disk;
  if (!loadHeader(disk) || !disk.pendingValid || !disk.journalCount ||
      disk.journalCount > static_cast<uint32_t>(INT32_MAX) / JournalEntry::kSize || disk.seq != header_.seq ||
      disk.recordCount != header_.recordCount || disk.journalCount != header_.journalCount ||
      disk.statDay != header_.statDay || disk.statNew != header_.statNew || disk.statReviews != header_.statReviews ||
      disk.pendingSlot != header_.pendingSlot || !(disk.pending == after) || disk.undoValid != entry.isReview() ||
      (disk.undoValid && (disk.undoSlot != disk.pendingSlot || !(disk.undoBefore == before))) ||
      store_.size(kItemsFile) != static_cast<int32_t>(kRecordsOffset + disk.recordCount * ItemState::kPackedSize) ||
      store_.size(kJournalFile) != static_cast<int32_t>(disk.journalCount * JournalEntry::kSize))
    return false;
  uint8_t journal[JournalEntry::kSize], expected[JournalEntry::kSize];
  entry.encode(expected);
  if (store_.read(kJournalFile, (disk.journalCount - 1) * JournalEntry::kSize, journal, sizeof(journal)) !=
          static_cast<int32_t>(sizeof(journal)) ||
      std::memcmp(journal, expected, sizeof(journal)) != 0)
    return false;
  ItemState stored;
  return readRecord(disk.pendingSlot, stored) && stored == after;
}

bool ProgressStore::loadUndoReview(JournalEntry& entry, ItemState& before, ItemState& after) {
  if (mode_ != Mode::Disk || !header_.undoValid || !header_.pendingValid || !header_.journalCount ||
      header_.journalCount > static_cast<uint32_t>(INT32_MAX) / JournalEntry::kSize)
    return false;
  uint8_t bytes[JournalEntry::kSize];
  if (store_.read(kJournalFile, (header_.journalCount - 1) * JournalEntry::kSize, bytes, sizeof(bytes)) !=
      static_cast<int32_t>(sizeof(bytes)))
    return false;
  const auto review = JournalEntry::decode(bytes);
  if (!review.isReview() || !verifyCommittedMutation(review, header_.undoBefore, header_.pending)) return false;
  entry = review;
  before = header_.undoBefore;
  after = header_.pending;
  return true;
}

bool ProgressStore::load(uint32_t index, ItemState& out) {
  if (index >= slotCount_ || index >= catalog_.itemCount()) return false;
  const uint16_t slot = slots_[index];
  if (slot == kNoSlot) {
    out = ItemState::fresh(catalog_.uidAt(index));
    return true;
  }
  return readRecord(slot & static_cast<uint16_t>(~kUnreviewedBit), out);
}

ProgressStore::ReviewResult ProgressStore::review(uint32_t index, Grade grade, uint8_t format, uint32_t responseMs,
                                                  DayNumber day, uint32_t time) {
  ReviewResult result;
  if (index >= slotCount_ || index >= catalog_.itemCount() || grade < Grade::Again || grade > Grade::Easy) {
    result.status = Status::Invalid;
    return result;
  }
  const JournalEntry entry = JournalEntry::review(catalog_.uidAt(index), grade, format, responseMs, day, time);
  Change change;
  result.status = apply(entry, change, responseMs);
  if (result.status == Status::Stored) {
    result.before = change.before;
    result.after = change.after;
    result.outcome = change.outcome;
  } else if (result.status == Status::NotStored) {
    // Graded but not kept: the session still needs the outcome.
    result.before = ItemState::fresh(entry.uid);
    result.after = result.before;
    result.outcome = applyReview(fsrs_, result.after, grade, day);
  }
  return result;
}

ProgressStore::Status ProgressStore::undo(DayNumber day, uint32_t time, ItemState* restored) {
  if (!canUndo()) return Status::Invalid;
  const JournalEntry entry = JournalEntry::control(header_.undoBefore.uid, JournalEntry::kUndo, 0, day, time);
  Change change;
  const Status status = apply(entry, change);
  if (status == Status::Stored && restored) *restored = change.after;
  return status;
}

ProgressStore::Status ProgressStore::setFlags(uint32_t index, uint16_t flags, DayNumber day, uint32_t time) {
  if (index >= slotCount_ || index >= catalog_.itemCount()) return Status::Invalid;
  const JournalEntry entry = JournalEntry::control(catalog_.uidAt(index), JournalEntry::kSetFlags,
                                                   static_cast<uint8_t>(flags & item_flag::kAll), day, time);
  Change change;
  return apply(entry, change);
}

bool ProgressStore::forecast(DayNumber today, uint16_t* counts, uint16_t days) {
  for (uint16_t i = 0; i < days; ++i) counts[i] = 0;
  return forEachRecord([&](const ItemState& s, int32_t index) {
    if (index < 0 || s.isNew() || s.suspended()) return;
    const uint32_t offset = s.dueDay <= today ? 0 : static_cast<uint32_t>(s.dueDay - today);
    if (offset < days) counts[offset] = saturatingIncrement(counts[offset]);
  });
}

bool ProgressStore::totals(Totals& out) {
  out = Totals();
  return forEachRecord([&](const ItemState& s, int32_t index) {
    if (index < 0) {
      ++out.retired;
      return;
    }
    if (s.suspended()) ++out.suspended;
    if (s.isNew()) return;
    ++out.seen;
    if (s.inSteps()) ++out.inSteps;
    if (s.stabilityDays() >= 21.0f) ++out.mature;
    if (s.flags & item_flag::kLeech) ++out.leeches;
  });
}

}  // namespace tinta::core

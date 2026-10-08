#pragma once

#include <cstdint>

#include "core/Clock.h"
#include "core/ItemCatalog.h"
#include "core/StateStore.h"
#include "core/srs/Fsrs.h"
#include "core/srs/ItemState.h"
#include "core/srs/Review.h"

namespace tinta::core {

// One record of reviews.log, 12 bytes little-endian:
//
//   0 uid u32   4 time u32 (Clock::nowSeconds)   8 day u16 (study day)
//  10 op u8    11 arg u8
//
// op bits 0-2 hold the grade (1..4) of a review, or 0 for a control record;
// bits 3-7 hold the exercise format of a review, or the control code. arg is
// the response time of a review in quarter seconds (0 unknown, saturating at
// 255 = 63.75 s), or the new flags of a set-flags record. The study day is
// stored rather than derived from the time because the X4's day is whatever
// the learner confirmed, and replay must see the day the review counted for.
struct JournalEntry {
  static constexpr uint32_t kSize = 12;
  static constexpr uint8_t kUndo = 1;      // undo the last review (uid: its item)
  static constexpr uint8_t kSetFlags = 2;  // replace an item's item_flag bits with arg

  uint32_t uid = 0;
  uint32_t time = 0;
  DayNumber day = 0;
  uint8_t op = 0;
  uint8_t arg = 0;

  static JournalEntry review(uint32_t uid, Grade grade, uint8_t format, uint32_t responseMs, DayNumber day,
                             uint32_t time);
  static JournalEntry control(uint32_t uid, uint8_t code, uint8_t arg, DayNumber day, uint32_t time);

  bool isReview() const { return (op & 0x07) >= 1 && (op & 0x07) <= 4; }
  Grade grade() const { return static_cast<Grade>(op & 0x07); }
  uint8_t format() const { return static_cast<uint8_t>(op >> 3); }
  uint8_t controlCode() const { return (op & 0x07) == 0 ? static_cast<uint8_t>(op >> 3) : 0; }

  void encode(uint8_t out[kSize]) const;
  static JournalEntry decode(const uint8_t in[kSize]);
};

// Learner progress over StateStore (PLAN.md 8.3): items.bin holds one 16-byte
// ItemState per item ever graded, in first-seen order; reviews.log journals
// every grade, undo and flag change.
//
// items.bin layout:
//
//   0     header copy A (80 bytes)
//   512   header copy B
//   1024  records, slot n at 1024 + 16 n
//
// The two header copies sit in different sectors and alternate; the valid one
// with the higher sequence number wins. A header carries the record and
// journal counts, today's new/review counters, the record it is about to
// write (pending) and the before-image of the last review (for undo).
//
// Each change costs three writes, in this order:
//   1. the journal record, at offset 12 × journalCount;
//   2. the header, which commits the change and names the record it implies;
//   3. the record itself, in place (or appended for a first sight).
// On open, the pending record is written again (idempotent), then any journal
// records past the header's count are replayed. A cut during 1 loses that
// review; during 2 the older header copy wins and the journal record is
// replayed; during 3 the pending record is redone. (A header copy damaged
// after its record was written, which a torn write cannot do since the next
// header goes to the other copy, would have that last change replayed twice.)
// If items.bin is missing or a record fails validation, items.bin is rebuilt
// from the whole journal; if the journal is shorter than the header says, or
// missing along with both headers (deleted or cut by hand), a rebuild would
// lose progress, so damaged records are dropped instead and the rest kept.
//
// Undo restores the before-image and journals an undo record (StateStore has
// no truncate). One level only: an undo or a flag change clears it. The
// before-image lives in the header, so an undo cut short by a power loss is
// replayed correctly, and a rebuild tracks it the same way.
//
// Without a card (StateStore::available() false) the store runs as a guest:
// records live in a caller-provided RAM array for this power cycle only, and
// a first sight beyond its capacity is graded but not stored.
//
// RAM: the caller's slot table (2 bytes per catalog item; 10 KB for 5,000
// items), the guest array if any (16 bytes per record), and 96 bytes of
// object state on the C3. Stack frames stay within 256 bytes.
class ProgressStore {
 public:
  static constexpr const char* kItemsFile = "items.bin";
  static constexpr const char* kJournalFile = "reviews.log";

  // Slot table entries: the record number, with kUnreviewedBit set when the
  // record exists but is in Phase::New (a flagged or undone new item).
  static constexpr uint16_t kNoSlot = 0xFFFF;
  static constexpr uint16_t kUnreviewedBit = 0x8000;
  static constexpr uint32_t kMaxRecords = 0x7FFF;
  // Written over a damaged record that could not be rebuilt; never a real
  // item (content must not assign this uid).
  static constexpr uint32_t kDeadUid = 0xFFFFFFFF;

  static constexpr uint32_t kHeaderSize = 80;
  static constexpr uint32_t kHeaderOffsetB = 512;
  static constexpr uint32_t kRecordsOffset = 1024;

  enum class OpenResult : uint8_t {
    Opened,    // clean
    Created,   // no progress yet; empty files created
    Replayed,  // journal records past the header were applied
    Rebuilt,   // items.bin was missing or corrupt and was rebuilt
    Salvaged,  // items.bin had damaged records and the log is too short to
               // rebuild from: the damaged records were dropped
    Guest,     // no card; RAM only
    Failed,    // I/O error or too small a slot table
  };

  enum class Status : uint8_t {
    Stored,     // durable (or, for a guest, held in RAM)
    NotStored,  // guest array full, or the record limit reached: graded, not kept
    Failed,     // I/O error; the store refuses changes until reopened
    Invalid,    // nothing to undo, or an index out of range
  };

  struct ReviewResult {
    Status status = Status::Failed;
    ItemState before;
    ItemState after;
    ReviewOutcome outcome;
  };

  struct Totals {
    uint32_t seen = 0;     // graded items in this catalog (not retired)
    uint32_t inSteps = 0;  // ...in learning or relearning
    uint32_t mature = 0;   // ...with stability of 21 days or more
    uint32_t suspended = 0;
    uint32_t leeches = 0;
    uint32_t retired = 0;  // records whose uid this catalog no longer has
  };

  ProgressStore(StateStore& store, const ItemCatalog& catalog, const Fsrs& fsrs);

  // `slots` needs catalog.itemCount() entries. `guestRecords` is used only
  // when the store is unavailable and may be null otherwise.
  OpenResult open(uint16_t* slots, uint32_t slotCount, ItemState* guestRecords, uint16_t guestCapacity);

  // Discards items.bin and replays the whole journal. open() does this when
  // it has to; exposed for tests and for a "repair" action.
  OpenResult rebuild();

  bool authoritativeRecoveryFailed() const { return authoritativeRecoveryFailed_; }
  bool isGuest() const { return mode_ == Mode::Guest; }
  bool failed() const { return mode_ == Mode::Failed || mode_ == Mode::Closed; }

  // Persist the authoritative event before local journal/derived-state writes.
  // The callback must log failures and keep its context alive through this store.
  struct MutationJournal {
    void* context = nullptr;
    bool (*persist)(void*, const JournalEntry&, const ItemState&, const ItemState&,
                    uint32_t responseMilliseconds) = nullptr;
    // Recover authoritative mutations into local files before open reads them.
    bool (*recover)(void*) = nullptr;
    // Acknowledge only after local disk state is durable; failure requires recovery.
    bool (*committed)(void*, const JournalEntry&, const ItemState&, const ItemState&,
                      uint32_t responseMilliseconds) = nullptr;
  };
  void setMutationJournal(MutationJournal journal) { mutationJournal_ = journal; }

  // Graded at least once (and not undone back to new).
  bool seen(uint32_t index) const {
    return index < slotCount_ && slots_[index] != kNoSlot && (slots_[index] & kUnreviewedBit) == 0;
  }
  bool hasRecord(uint32_t index) const { return index < slotCount_ && slots_[index] != kNoSlot; }

  // An item's state; ItemState::fresh(uid) if it has no record. False on I/O
  // error or a bad index.
  bool load(uint32_t index, ItemState& out);

  // Read-only proof of the latest disk mutation. Caller excludes writers.
  // Does not validate companion authority or earlier journal history.
  bool verifyCommittedMutation(const JournalEntry& entry, const ItemState& before, const ItemState& after);

  // Grades item `index` on `day`. `time` is Clock::nowSeconds().
  ReviewResult review(uint32_t index, Grade grade, uint8_t format, uint32_t responseMs, DayNumber day, uint32_t time);

  bool canUndo() const { return mode_ != Mode::Failed && header_.undoValid; }
  // Read-only native proof for companion undo-identity recovery. Outputs change
  // only on success; caller excludes mutations and validates companion authority.
  bool loadUndoReview(JournalEntry& entry, ItemState& before, ItemState& after);
  // Undoes the last review; `restored` receives the item's state as it was.
  Status undo(DayNumber day, uint32_t time, ItemState* restored = nullptr);

  // Replaces the item_flag bits of item `index` (creating a Phase::New
  // record for an unseen item, so a new item can be suspended).
  Status setFlags(uint32_t index, uint16_t flags, DayNumber day, uint32_t time);

  // Items first graded / due items graded on `day` (zero for any other day).
  uint16_t newOn(DayNumber day) const { return header_.statDay == day ? header_.statNew : 0; }
  uint16_t reviewsOn(DayNumber day) const { return header_.statDay == day ? header_.statReviews : 0; }

  uint32_t recordCount() const { return header_.recordCount; }
  uint32_t journalCount() const { return header_.journalCount; }
  // Canonical companion snapshots retain this day even with an empty local log.
  DayNumber lastStudyDay() const { return header_.statDay; }

  // Calls visit(const ItemState&, int32_t index) for every record in slot
  // order; index is -1 for a retired uid. One sequential pass over items.bin.
  template <class Visit>
  bool forEachRecord(Visit&& visit);

  // counts[d] = items due on today + d; overdue items count for today.
  // Suspended and new items are left out.
  bool forecast(DayNumber today, uint16_t* counts, uint16_t days);

  bool totals(Totals& out);

 private:
  enum class Mode : uint8_t { Closed, Disk, Guest, Failed };
  enum class Plan : uint8_t { Ok, Invalid, NoRoom, IoError };
  enum class Map : uint8_t { Ok, Salvaged, Corrupt, IoError };

  struct Header {
    uint32_t seq = 0;
    uint32_t recordCount = 0;
    uint32_t journalCount = 0;
    DayNumber statDay = 0;
    uint16_t statNew = 0;
    uint16_t statReviews = 0;
    bool pendingValid = false;
    bool undoValid = false;
    uint32_t pendingSlot = 0;
    ItemState pending;
    uint32_t undoSlot = 0;
    ItemState undoBefore;
    DayNumber undoStatDay = 0;
    uint16_t undoStatNew = 0;
    uint16_t undoStatReviews = 0;
  };

  struct Change {
    Header next;
    uint32_t slot = 0;
    int32_t index = -1;
    ItemState before;
    ItemState after;
    ReviewOutcome outcome;
  };

  static constexpr uint32_t kChunkRecords = 8;

  static void encodeHeader(const Header& h, uint8_t out[kHeaderSize]);
  static bool decodeHeader(const uint8_t in[kHeaderSize], Header& out);

  Plan plan(const JournalEntry& entry, bool allowUnknownUid, Change& out);
  bool commit(const Change& change);
  Status apply(const JournalEntry& entry, Change& change, uint32_t responseMilliseconds = 0);
  bool resolveSlot(uint32_t uid, int32_t& index, uint32_t& slot, bool& exists);
  bool findRecord(uint32_t uid, uint32_t& slot);
  bool readRecord(uint32_t slot, ItemState& out);
  bool readRecords(uint32_t first, uint32_t count, uint8_t* buffer);
  bool writeRecord(uint32_t slot, const ItemState& state);
  bool writeHeader(Header& h);
  bool loadHeader(Header& out);
  Map mapRecords(bool salvage);
  OpenResult salvage(uint32_t journalCount, bool damaged);
  uint32_t wholeRecords();
  uint32_t journalRecords();
  bool replayTail(bool& replayed);
  bool createEmpty();
  bool zeroRange(const char* name, uint32_t from, uint32_t to, bool onlyIfDirty);
  void clearSlots();
  OpenResult fail();

  StateStore& store_;
  const ItemCatalog& catalog_;
  const Fsrs& fsrs_;
  uint16_t* slots_ = nullptr;
  uint32_t slotCount_ = 0;
  ItemState* guest_ = nullptr;
  uint16_t guestCapacity_ = 0;
  Mode mode_ = Mode::Closed;
  MutationJournal mutationJournal_;
  bool authoritativeRecoveryFailed_ = false;
  bool rebuilding_ = false;
  Header header_;
};

template <class Visit>
bool ProgressStore::forEachRecord(Visit&& visit) {
  if (mode_ == Mode::Guest) {
    for (uint32_t i = 0; i < header_.recordCount; ++i) {
      visit(static_cast<const ItemState&>(guest_[i]), catalog_.indexOfUid(guest_[i].uid));
    }
    return true;
  }
  if (mode_ != Mode::Disk) return false;
  uint8_t buffer[kChunkRecords * ItemState::kPackedSize];
  for (uint32_t first = 0; first < header_.recordCount; first += kChunkRecords) {
    uint32_t n = header_.recordCount - first;
    if (n > kChunkRecords) n = kChunkRecords;
    if (!readRecords(first, n, buffer)) return false;
    for (uint32_t i = 0; i < n; ++i) {
      ItemState state;
      if (!ItemState::decode(buffer + i * ItemState::kPackedSize, state)) continue;
      if (state.uid == kDeadUid) continue;
      visit(static_cast<const ItemState&>(state), catalog_.indexOfUid(state.uid));
    }
  }
  return true;
}

}  // namespace tinta::core

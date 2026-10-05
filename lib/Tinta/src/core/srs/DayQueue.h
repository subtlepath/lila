#pragma once

#include <cstdint>

#include "core/Clock.h"
#include "core/ItemCatalog.h"
#include "core/StateStore.h"
#include "core/srs/Fsrs.h"
#include "core/srs/ItemState.h"
#include "core/srs/ProgressStore.h"

namespace tinta::core {

// One slot of the session queue. 6 bytes; the caller owns the array.
struct QueueEntry {
  uint16_t index = 0;  // catalog index
  uint16_t rank = 0;   // build-time sort key: 0 for items in their steps, else 1 + R scaled
  uint8_t flags = 0;   // kEntryNew, kEntryStep
  uint8_t reserved = 0;
};

constexpr uint8_t kEntryNew = 1u << 0;   // first showing of a new item
constexpr uint8_t kEntryStep = 1u << 1;  // back for a learning or relearning step

// ItemCatalog::lessonAt() for items outside the lessons: the frequency list
// and the phrasebook (docs/pack-format.md 3.9).
constexpr uint16_t kNoLesson = 0xFFFF;

enum class SessionKind : uint8_t {
  Today = 0,     // build(): due reviews, then new items
  Practice = 1,  // buildPractice(): a caller's list (a lesson, a phrasebook category)
};

struct SessionLimits {
  uint16_t newPerDay = 10;            // new items per study day, across sessions
  uint16_t reviewCap = 100;           // due items graded per study day, across sessions
  uint16_t sessionSize = 40;          // items in one session; 0 = the buffer's capacity
  uint16_t unlockedThrough = 0xFFFF;  // new lesson items only from lessons up to this one
  // Items the learner is not to see (the app's "show vulgar words" off):
  // left out of due reviews and new items alike, their state untouched.
  // Null: none.
  bool (*excluded)(void* context, uint32_t index) = nullptr;
  void* excludedContext = nullptr;
  // New items to bring in before any other, whatever the lessons unlocked
  // (the learner's starred words, PLAN.md M6): catalog indices, in order.
  // Only new, unsuspended, not excluded ones are taken; they need only room
  // in the session (newPerDay does not hold them back) and then count toward
  // the day's new items like any other. Null: none.
  const uint32_t* first = nullptr;
  uint16_t firstCount = 0;
};

// What a session adds to the day's totals (stats/DayLog keeps them).
struct StudyTotals {
  uint16_t reviews = 0;   // grades given
  uint16_t correct = 0;   // grades other than Again
  uint16_t newItems = 0;  // first grades of new items
  uint32_t milliseconds = 0;
};

struct IntervalPreview {
  bool inSession = false;  // comes back later in this session
  uint16_t days = 0;       // otherwise due in this many days
};

struct QueueCounts {
  uint16_t remaining = 0;  // items still to show (includes step returns)
  uint16_t newLeft = 0;    // ...of which new items not yet shown
  uint16_t stepsLeft = 0;  // ...of which items back for a step
  uint16_t dueLeft = 0;    // ...of which due reviews
  uint16_t done = 0;       // grades given this session
};

struct AnswerResult {
  ProgressStore::Status status = ProgressStore::Status::Invalid;
  bool inSession = false;
  uint16_t intervalDays = 0;
  bool lapse = false;
  bool becameLeech = false;  // show the note and offer suspend()
};

// The Today session (PLAN.md 8.1, 8.2 steps 1-3).
//
// build() scans items.bin once for items due on the study day (of any kind),
// puts items left in their steps first, then the rest by retrievability
// (lowest first) up to what remains of the day's review cap. Then it adds new
// items, in catalog order, up to what remains of the day's new limit: lesson
// items from unlocked lessons first, and frequency-list items (kNoLesson)
// only for what the lessons leave. Phrase items are never introduced here. A
// gated item (prerequisiteOf, a word's produce item) is added only once its
// prerequisite's stability reaches kPrerequisiteStabilityDays. Finally the
// order is adjusted so that two items of one word (sharing a prerequisite
// root) are not adjacent.
//
// buildPractice() takes the caller's list instead: seen and new items alike,
// with no limits, gating or due dates. Everything else is the same: steps,
// grading, journal, undo, resume. New items graded there count toward the
// day's new total, so a later build() introduces fewer.
//
// answer() grades the current item through ProgressStore. An item still in
// its steps goes back into the queue with at least ReviewOutcome::gap other
// items ahead of it (fewer if the queue is shorter); otherwise it leaves.
// undo() takes back the last answer, in the store and in the queue.
//
// All storage is the caller's QueueEntry array; a session never grows past
// what build() put in it. 6 bytes per entry (768 bytes for 128) plus 68 bytes
// of object state on the C3. Catalog indices are 16-bit: up to 65,536 items.
// session.bin holds the queue by uid (5 bytes an item, plus 32) so that a
// session survives sleep, and is valid only on the study day it was built.
class DayQueue {
 public:
  static constexpr const char* kSessionFile = "session.bin";
  static constexpr float kPrerequisiteStabilityDays = 3.0f;

  DayQueue(ProgressStore& progress, const ItemCatalog& catalog, const Fsrs& fsrs, const Clock& clock,
           QueueEntry* entries, uint16_t capacity);

  // Builds a new session for clock.today(). False on a storage error.
  bool build(const SessionLimits& limits);

  // Builds a practice session for clock.today() from catalog `indices`, in
  // that order apart from keeping a word's items apart. Suspended items,
  // repeats and bad indices are skipped; at most the buffer's capacity is
  // taken. `tag` is the caller's (say, the lesson or category) and is kept
  // across resume. False on a storage error.
  bool buildPractice(const uint32_t* indices, uint32_t count, uint16_t tag = 0);

  SessionKind kind() const { return kind_; }
  uint16_t tag() const { return tag_; }

  DayNumber day() const { return day_; }
  bool empty() const { return count_ == 0; }
  // Catalog index of the item to show now, or -1.
  int32_t current() const { return count_ ? entries_[0].index : -1; }
  bool currentIsNew() const { return count_ && (entries_[0].flags & kEntryNew); }
  const QueueEntry* entries() const { return entries_; }
  uint16_t size() const { return count_; }

  // Where each grade would send the current item: out[grade - 1].
  bool preview(IntervalPreview out[4]);

  AnswerResult answer(Grade grade, uint8_t format, uint32_t responseMs);

  bool canUndo() const { return undo_.valid && progress_.canUndo(); }
  bool undo();

  // Suspends item `index` and drops it from the session.
  ProgressStore::Status suspend(uint32_t index);

  QueueCounts counts() const;

  // Totals since the last call, for DayLog. Clears undo, so a later undo
  // cannot take back a review that has already been counted.
  StudyTotals takeTotals();

  // session.bin. `scratch` must hold blobSize(capacity) bytes.
  static constexpr uint32_t blobSize(uint16_t capacity) { return 32u + 5u * capacity; }
  uint32_t serialize(uint8_t* out, uint32_t capacity) const;
  // False (and an empty queue) when the blob is corrupt or from another day.
  bool restore(const uint8_t* in, uint32_t length);
  bool save(StateStore& store, uint8_t* scratch, uint32_t scratchSize) const;
  bool load(StateStore& store, uint8_t* scratch, uint32_t scratchSize);

 private:
  struct Undo {
    bool valid = false;
    QueueEntry entry;
    int32_t insertedAt = -1;
    uint16_t done = 0;
    StudyTotals totals;
  };

  uint32_t group(uint32_t index) const;
  bool related(const QueueEntry& a, const QueueEntry& b) const;
  void insertAt(uint16_t position, const QueueEntry& entry);
  void removeAt(uint16_t position);
  uint16_t placeFor(const QueueEntry& entry, uint8_t gap) const;
  void reset();
  void spread();
  bool addNewItems(const SessionLimits& limits, uint16_t room);
  bool canIntroduce(uint32_t index, bool& ok);
  bool queued(uint32_t index) const;
  void heapPush(uint16_t& size, uint16_t limit, const QueueEntry& entry);
  void heapSort(uint16_t size);

  ProgressStore& progress_;
  const ItemCatalog& catalog_;
  const Fsrs& fsrs_;
  const Clock& clock_;
  QueueEntry* entries_;
  uint16_t capacity_;
  uint16_t count_ = 0;
  uint16_t done_ = 0;
  DayNumber day_ = 0;
  SessionKind kind_ = SessionKind::Today;
  uint16_t tag_ = 0;
  StudyTotals totals_;
  Undo undo_;
};

}  // namespace tinta::core

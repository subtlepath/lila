#include "core/srs/DayQueue.h"

#include <cstring>

#include "core/srs/Bytes.h"
#include "core/srs/Review.h"

namespace tinta::core {

namespace {

constexpr uint8_t kMagic[4] = {'T', 'S', 'Q', '1'};
constexpr uint16_t kVersion = 2;
constexpr uint32_t kBlobHeaderV1 = 24;  // version 1: no kind or tag
constexpr uint32_t kBlobHeader = 28;
constexpr uint32_t kBlobEntry = 5;
constexpr int kMaxPrerequisiteHops = 4;

bool before(const QueueEntry& a, const QueueEntry& b) { return a.rank != b.rank ? a.rank < b.rank : a.index < b.index; }

// Places `value` at the root of a max-heap of `size` entries and sifts it down.
void siftDown(QueueEntry* heap, uint16_t size, QueueEntry value) {
  uint32_t i = 0;
  for (;;) {
    uint32_t child = 2 * i + 1;
    if (child >= size) break;
    if (child + 1 < size && before(heap[child], heap[child + 1])) ++child;
    if (!before(value, heap[child])) break;
    heap[i] = heap[child];
    i = child;
  }
  heap[i] = value;
}

uint16_t saturatingAdd(uint16_t a, uint32_t b) {
  const uint32_t sum = a + b;
  return sum > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(sum);
}

}  // namespace

DayQueue::DayQueue(ProgressStore& progress, const ItemCatalog& catalog, const Fsrs& fsrs, const Clock& clock,
                   QueueEntry* entries, uint16_t capacity)
    : progress_(progress),
      catalog_(catalog),
      fsrs_(fsrs),
      clock_(clock),
      entries_(entries),
      capacity_(entries ? capacity : 0) {}

// --- Building ---------------------------------------------------------------

// Max-heap on (rank, index) in entries_[0..size): keeps the `limit` lowest.
void DayQueue::heapPush(uint16_t& size, uint16_t limit, const QueueEntry& entry) {
  if (limit == 0) return;
  if (size == limit) {
    if (before(entry, entries_[0])) siftDown(entries_, size, entry);
    return;
  }
  uint16_t i = size++;
  while (i > 0) {
    const uint16_t parent = static_cast<uint16_t>((i - 1) / 2);
    if (!before(entries_[parent], entry)) break;
    entries_[i] = entries_[parent];
    i = parent;
  }
  entries_[i] = entry;
}

void DayQueue::heapSort(uint16_t size) {
  for (uint16_t end = size; end > 1; --end) {
    const QueueEntry top = entries_[0];
    siftDown(entries_, static_cast<uint16_t>(end - 1), entries_[end - 1]);
    entries_[end - 1] = top;
  }
}

void DayQueue::reset() {
  count_ = 0;
  done_ = 0;
  totals_ = StudyTotals();
  undo_ = Undo();
  kind_ = SessionKind::Today;
  tag_ = 0;
  day_ = clock_.today();
}

bool DayQueue::build(const SessionLimits& limits) {
  reset();

  uint16_t room = capacity_;
  if (limits.sessionSize != 0 && limits.sessionSize < room) room = limits.sessionSize;

  // 1. Due items: the `room` lowest ranks, items in their steps first.
  uint16_t heap = 0;
  const DayNumber day = day_;
  const bool scanned = progress_.forEachRecord([&](const ItemState& s, int32_t index) {
    if (index < 0 || index > 0xFFFF || s.isNew() || s.suspended() || s.dueDay > day) return;
    if (limits.excluded && limits.excluded(limits.excludedContext, static_cast<uint32_t>(index))) return;
    QueueEntry e;
    e.index = static_cast<uint16_t>(index);
    if (s.inSteps()) {
      e.rank = 0;
      e.flags = kEntryStep;
    } else {
      const uint32_t elapsed = day > s.lastDay ? static_cast<uint32_t>(day - s.lastDay) : 0;
      const float r = fsrs_.retrievability(s.stabilityDays(), elapsed);
      e.rank = static_cast<uint16_t>(1 + static_cast<uint16_t>(r * 60000.0f));
    }
    heapPush(heap, room, e);
  });
  if (!scanned) return false;
  heapSort(heap);

  // The review cap applies to due reviews, not to items finishing their steps.
  const uint16_t reviewedToday = progress_.reviewsOn(day_);
  uint32_t reviewRoom = limits.reviewCap > reviewedToday ? limits.reviewCap - reviewedToday : 0;
  for (uint16_t i = 0; i < heap; ++i) {
    if (entries_[i].flags & kEntryStep) {
      entries_[count_++] = entries_[i];
    } else if (reviewRoom > 0) {
      entries_[count_++] = entries_[i];
      --reviewRoom;
    }
  }

  // 2. New items.
  if (!addNewItems(limits, room)) return false;

  // 3. Keep a word's items apart.
  spread();
  return true;
}

// Whether new item `index` may be introduced now: not suspended, and its
// prerequisite (if any) learnt to kPrerequisiteStabilityDays. False with
// ok == false on a storage error.
bool DayQueue::canIntroduce(uint32_t index, bool& ok) {
  ok = true;
  ItemState state;
  if (progress_.hasRecord(index)) {
    if (!progress_.load(index, state)) return ok = false;
    if (state.suspended()) return false;
  }
  const int32_t prerequisite = catalog_.prerequisiteOf(index);
  if (prerequisite < 0) return true;
  if (!progress_.seen(static_cast<uint32_t>(prerequisite))) return false;
  if (!progress_.load(static_cast<uint32_t>(prerequisite), state)) return ok = false;
  return state.stabilityDays() >= kPrerequisiteStabilityDays;
}

bool DayQueue::addNewItems(const SessionLimits& limits, uint16_t room) {
  const uint32_t items = catalog_.itemCount() <= 0x10000 ? catalog_.itemCount() : 0x10000;
  // The learner's own choices first.
  uint16_t firstAdded = 0;
  for (uint16_t k = 0; k < limits.firstCount && limits.first && count_ < room; ++k) {
    const uint32_t i = limits.first[k];
    if (i >= items || progress_.seen(i) || queued(i)) continue;
    if (limits.excluded && limits.excluded(limits.excludedContext, i)) continue;
    bool ok;
    const bool introduce = canIntroduce(i, ok);
    if (!ok) return false;
    if (!introduce) continue;
    QueueEntry e;
    e.index = static_cast<uint16_t>(i);
    e.flags = kEntryNew;
    entries_[count_++] = e;
    ++firstAdded;
  }
  const uint16_t newToday = static_cast<uint16_t>(progress_.newOn(day_) + firstAdded);
  uint32_t newRoom = limits.newPerDay > newToday ? limits.newPerDay - newToday : 0;
  if (newRoom > static_cast<uint32_t>(room - count_)) newRoom = room - count_;
  // Pass 0: unlocked lessons. Pass 1: the frequency list, for what is left.
  for (int pass = 0; pass < 2 && newRoom > 0; ++pass) {
    for (uint32_t i = 0; i < items && newRoom > 0; ++i) {
      if (catalog_.kindAt(i) == ItemKind::Phrase) continue;  // learnt from the phrasebook
      const uint16_t lesson = catalog_.lessonAt(i);
      const bool wanted = pass == 0 ? lesson != kNoLesson && lesson <= limits.unlockedThrough : lesson == kNoLesson;
      if (!wanted || progress_.seen(i)) continue;
      if (limits.excluded && limits.excluded(limits.excludedContext, i)) continue;
      if (firstAdded > 0 && queued(i)) continue;
      bool ok;
      const bool introduce = canIntroduce(i, ok);
      if (!ok) return false;
      if (!introduce) continue;
      QueueEntry e;
      e.index = static_cast<uint16_t>(i);
      e.flags = kEntryNew;
      entries_[count_++] = e;
      --newRoom;
    }
  }
  return true;
}

bool DayQueue::queued(uint32_t index) const {
  for (uint16_t i = 0; i < count_; ++i) {
    if (entries_[i].index == index) return true;
  }
  return false;
}

bool DayQueue::buildPractice(const uint32_t* indices, uint32_t count, uint16_t tag) {
  reset();
  kind_ = SessionKind::Practice;
  tag_ = tag;
  for (uint32_t k = 0; k < count && count_ < capacity_; ++k) {
    const uint32_t index = indices[k];
    if (index >= catalog_.itemCount() || index > 0xFFFF) continue;
    bool repeat = false;
    for (uint16_t i = 0; i < count_ && !repeat; ++i) repeat = entries_[i].index == index;
    if (repeat) continue;
    ItemState state;
    if (!progress_.load(index, state)) return false;
    if (state.suspended()) continue;
    QueueEntry e;
    e.index = static_cast<uint16_t>(index);
    e.flags = state.isNew() ? kEntryNew : state.inSteps() ? kEntryStep : 0;
    entries_[count_++] = e;
  }
  spread();
  return true;
}

uint32_t DayQueue::group(uint32_t index) const {
  for (int hop = 0; hop < kMaxPrerequisiteHops; ++hop) {
    const int32_t p = catalog_.prerequisiteOf(index);
    if (p < 0) break;
    index = static_cast<uint32_t>(p);
  }
  return index;
}

bool DayQueue::related(const QueueEntry& a, const QueueEntry& b) const {
  return a.index == b.index || group(a.index) == group(b.index);
}

void DayQueue::spread() {
  for (uint16_t p = 1; p < count_; ++p) {
    if (!related(entries_[p - 1], entries_[p])) continue;
    for (uint16_t q = static_cast<uint16_t>(p + 1); q < count_; ++q) {
      if (related(entries_[p - 1], entries_[q])) continue;
      const QueueEntry moved = entries_[q];
      std::memmove(entries_ + p + 1, entries_ + p, (q - p) * sizeof(QueueEntry));
      entries_[p] = moved;
      break;
    }
  }
}

// --- In session -------------------------------------------------------------

void DayQueue::insertAt(uint16_t position, const QueueEntry& entry) {
  if (count_ >= capacity_) return;
  if (position > count_) position = count_;
  std::memmove(entries_ + position + 1, entries_ + position, (count_ - position) * sizeof(QueueEntry));
  entries_[position] = entry;
  ++count_;
}

void DayQueue::removeAt(uint16_t position) {
  if (position >= count_) return;
  std::memmove(entries_ + position, entries_ + position + 1, (count_ - position - 1) * sizeof(QueueEntry));
  --count_;
}

// The first position at least `gap` items back (or the end) whose neighbours
// are not the same word; failing that, the plain position.
uint16_t DayQueue::placeFor(const QueueEntry& entry, uint8_t gap) const {
  const uint16_t start = gap < count_ ? gap : count_;
  for (uint16_t p = start; p <= count_; ++p) {
    const bool clashBefore = p > 0 && related(entries_[p - 1], entry);
    const bool clashAfter = p < count_ && related(entries_[p], entry);
    if (!clashBefore && !clashAfter) return p;
  }
  return start;
}

bool DayQueue::preview(IntervalPreview out[4]) {
  if (count_ == 0) return false;
  ItemState state;
  if (!progress_.load(entries_[0].index, state)) return false;
  for (uint8_t g = 1; g <= 4; ++g) {
    ItemState copy = state;
    const ReviewOutcome o = applyReview(fsrs_, copy, static_cast<Grade>(g), day_);
    out[g - 1].inSession = o.inSession;
    out[g - 1].days = o.intervalDays;
  }
  return true;
}

AnswerResult DayQueue::answer(Grade grade, uint8_t format, uint32_t responseMs) {
  AnswerResult result;
  if (count_ == 0) return result;
  const QueueEntry entry = entries_[0];
  const ProgressStore::ReviewResult r =
      progress_.review(entry.index, grade, format, responseMs, day_, clock_.nowSeconds());
  result.status = r.status;
  if (r.status == ProgressStore::Status::Failed || r.status == ProgressStore::Status::Invalid) {
    return result;
  }
  result.inSession = r.outcome.inSession;
  result.intervalDays = r.outcome.intervalDays;
  result.lapse = r.outcome.lapse;
  result.becameLeech = r.outcome.becameLeech;

  Undo undo;
  undo.valid = r.status == ProgressStore::Status::Stored;
  undo.entry = entry;
  undo.done = done_;
  undo.totals = totals_;

  removeAt(0);
  if (r.outcome.inSession) {
    QueueEntry back = entry;
    back.flags = kEntryStep;
    const uint16_t position = placeFor(back, r.outcome.gap);
    insertAt(position, back);
    undo.insertedAt = position;
  }
  undo_ = undo;

  done_ = saturatingAdd(done_, 1);
  totals_.reviews = saturatingAdd(totals_.reviews, 1);
  if (grade != Grade::Again) totals_.correct = saturatingAdd(totals_.correct, 1);
  if (r.before.isNew()) totals_.newItems = saturatingAdd(totals_.newItems, 1);
  totals_.milliseconds += responseMs;
  return result;
}

bool DayQueue::undo() {
  if (!canUndo()) return false;
  if (progress_.undo(day_, clock_.nowSeconds()) != ProgressStore::Status::Stored) return false;
  if (undo_.insertedAt >= 0) removeAt(static_cast<uint16_t>(undo_.insertedAt));
  insertAt(0, undo_.entry);
  done_ = undo_.done;
  totals_ = undo_.totals;
  undo_ = Undo();
  return true;
}

ProgressStore::Status DayQueue::suspend(uint32_t index) {
  ItemState state;
  if (!progress_.load(index, state)) return ProgressStore::Status::Failed;
  const ProgressStore::Status status = progress_.setFlags(
      index, static_cast<uint16_t>(state.flags | item_flag::kSuspended), clock_.today(), clock_.nowSeconds());
  if (status != ProgressStore::Status::Stored) return status;
  for (uint16_t i = count_; i > 0; --i) {
    if (entries_[i - 1].index == index) removeAt(static_cast<uint16_t>(i - 1));
  }
  undo_ = Undo();
  return status;
}

QueueCounts DayQueue::counts() const {
  QueueCounts c;
  c.remaining = count_;
  c.done = done_;
  for (uint16_t i = 0; i < count_; ++i) {
    if (entries_[i].flags & kEntryNew) {
      ++c.newLeft;
    } else if (entries_[i].flags & kEntryStep) {
      ++c.stepsLeft;
    } else {
      ++c.dueLeft;
    }
  }
  return c;
}

StudyTotals DayQueue::takeTotals() {
  const StudyTotals t = totals_;
  totals_ = StudyTotals();
  undo_ = Undo();
  return t;
}

// --- session.bin ------------------------------------------------------------
//
//   0 magic "TSQ1"  4 version u16 (2)  6 count u16  8 day u16  10 done u16
//  12 reviews u16  14 correct u16  16 newItems u16  18 kind u8  19 zero u8
//  20 ms u32       24 tag u16  26 zero u16
//  28 count × (uid u32, flags u8)
//  then crc32 of everything before it
// Version 1 had no kind or tag and its entries started at 24; it reads back
// as a Today session.

uint32_t DayQueue::serialize(uint8_t* out, uint32_t capacity) const {
  const uint32_t size = kBlobHeader + kBlobEntry * count_ + 4;
  if (capacity < size) return 0;
  std::memset(out, 0, kBlobHeader);
  std::memcpy(out, kMagic, 4);
  putU16(out + 4, kVersion);
  putU16(out + 6, count_);
  putU16(out + 8, day_);
  putU16(out + 10, done_);
  putU16(out + 12, totals_.reviews);
  putU16(out + 14, totals_.correct);
  putU16(out + 16, totals_.newItems);
  out[18] = static_cast<uint8_t>(kind_);
  putU32(out + 20, totals_.milliseconds);
  putU16(out + 24, tag_);
  uint8_t* p = out + kBlobHeader;
  for (uint16_t i = 0; i < count_; ++i, p += kBlobEntry) {
    putU32(p, catalog_.uidAt(entries_[i].index));
    p[4] = entries_[i].flags;
  }
  putU32(p, crc32(out, size - 4));
  return size;
}

bool DayQueue::restore(const uint8_t* in, uint32_t length) {
  reset();
  if (length < kBlobHeaderV1 + 4 || std::memcmp(in, kMagic, 4) != 0) return false;
  const uint16_t version = getU16(in + 4);
  if (version != 1 && version != kVersion) return false;
  const uint32_t header = version == 1 ? kBlobHeaderV1 : kBlobHeader;
  const uint16_t n = getU16(in + 6);
  const uint32_t size = header + kBlobEntry * n + 4;
  if (size > length || getU32(in + size - 4) != crc32(in, size - 4)) return false;
  if (getU16(in + 8) != clock_.today()) return false;
  if (version != 1) {
    if (in[18] > static_cast<uint8_t>(SessionKind::Practice)) return false;
    kind_ = static_cast<SessionKind>(in[18]);
    tag_ = getU16(in + 24);
  }

  day_ = getU16(in + 8);
  done_ = getU16(in + 10);
  totals_.reviews = getU16(in + 12);
  totals_.correct = getU16(in + 14);
  totals_.newItems = getU16(in + 16);
  totals_.milliseconds = getU32(in + 20);
  const uint8_t* p = in + header;
  for (uint16_t i = 0; i < n && count_ < capacity_; ++i, p += kBlobEntry) {
    // Items may have been retired by a content update while asleep.
    const int32_t index = catalog_.indexOfUid(getU32(p));
    if (index < 0 || index > 0xFFFF) continue;
    QueueEntry e;
    e.index = static_cast<uint16_t>(index);
    e.flags = p[4] & (kEntryNew | kEntryStep);
    if ((e.flags & kEntryNew) && progress_.seen(e.index)) e.flags = kEntryStep;
    entries_[count_++] = e;
  }
  return true;
}

bool DayQueue::save(StateStore& store, uint8_t* scratch, uint32_t scratchSize) const {
  if (!store.available()) return false;
  const uint32_t size = serialize(scratch, scratchSize);
  return size != 0 && store.replace(kSessionFile, scratch, size);
}

bool DayQueue::load(StateStore& store, uint8_t* scratch, uint32_t scratchSize) {
  const int32_t size = store.size(kSessionFile);
  if (size <= 0 || static_cast<uint32_t>(size) > scratchSize) {
    restore(scratch, 0);
    return false;
  }
  if (store.read(kSessionFile, 0, scratch, static_cast<uint32_t>(size)) != size) {
    restore(scratch, 0);
    return false;
  }
  return restore(scratch, static_cast<uint32_t>(size));
}

}  // namespace tinta::core

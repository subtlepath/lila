#include "core/stats/DayLog.h"

#include <cstring>

#include "core/srs/Bytes.h"

namespace tinta::core {

namespace {

constexpr uint8_t kMagic[4] = {'T', 'D', 'L', '1'};
constexpr uint32_t kChunk = 8;

uint16_t check(const uint8_t* record) { return static_cast<uint16_t>(crc32(record, 10)); }

uint16_t take16(uint32_t& v) {
  const uint32_t part = v > 0xFFFF ? 0xFFFF : v;
  v -= part;
  return static_cast<uint16_t>(part);
}

}  // namespace

template <class Visit>
bool DayLog::forEach(Visit&& visit) {
  if (!store_.available()) {
    if (guestTotals_.reviews > 0 || guestTotals_.seconds > 0) visit(guestDay_, guestTotals_);
    return true;
  }
  const int32_t size = store_.size(kFile);
  if (size < 0) return true;  // nothing logged yet
  uint8_t buffer[kChunk * kRecordSize];
  if (size < static_cast<int32_t>(kHeaderSize) ||
      store_.read(kFile, 0, buffer, kHeaderSize) != static_cast<int32_t>(kHeaderSize) ||
      std::memcmp(buffer, kMagic, kHeaderSize) != 0) {
    return true;  // not a log this firmware wrote; add() starts a new one
  }
  const uint32_t count = (static_cast<uint32_t>(size) - kHeaderSize) / kRecordSize;
  for (uint32_t first = 0; first < count; first += kChunk) {
    uint32_t n = count - first;
    if (n > kChunk) n = kChunk;
    const int32_t bytes = static_cast<int32_t>(n * kRecordSize);
    if (store_.read(kFile, kHeaderSize + first * kRecordSize, buffer, static_cast<uint32_t>(bytes)) != bytes) {
      return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* r = buffer + i * kRecordSize;
      if (getU16(r + 10) != check(r)) continue;  // torn: that session is lost
      DayTotals t;
      t.reviews = getU16(r + 2);
      t.correct = getU16(r + 4);
      t.newItems = getU16(r + 6);
      t.seconds = getU16(r + 8);
      visit(static_cast<DayNumber>(getU16(r)), t);
    }
  }
  return true;
}

bool DayLog::writeRecord(DayNumber day, const DayTotals& part) {
  uint8_t r[kRecordSize];
  putU16(r, day);
  putU16(r + 2, static_cast<uint16_t>(part.reviews));
  putU16(r + 4, static_cast<uint16_t>(part.correct));
  putU16(r + 6, static_cast<uint16_t>(part.newItems));
  putU16(r + 8, static_cast<uint16_t>(part.seconds));
  putU16(r + 10, check(r));

  // size() is -1 for a missing file and for an I/O error alike; the platform
  // store reports itself unavailable after an error. Only a file that is
  // really missing, too short or someone else's is started afresh.
  const int32_t size = store_.size(kFile);
  bool fresh = size < static_cast<int32_t>(kHeaderSize);
  if (!fresh) {
    uint8_t magic[kHeaderSize];
    if (store_.read(kFile, 0, magic, kHeaderSize) != static_cast<int32_t>(kHeaderSize)) return false;
    fresh = std::memcmp(magic, kMagic, kHeaderSize) != 0;
  }
  if (fresh) {
    if (!store_.available()) return false;
    uint8_t file[kHeaderSize + kRecordSize];
    std::memcpy(file, kMagic, kHeaderSize);
    std::memcpy(file + kHeaderSize, r, kRecordSize);
    return store_.replace(kFile, file, sizeof file);
  }
  // Written at the record boundary so a torn earlier append is overwritten.
  const uint32_t at = kHeaderSize + (static_cast<uint32_t>(size) - kHeaderSize) / kRecordSize * kRecordSize;
  return store_.write(kFile, at, r, kRecordSize);
}

bool DayLog::add(DayNumber day, const DayTotals& delta) {
  if (!store_.available()) {
    if (guestDay_ != day) guestTotals_ = DayTotals();
    guestDay_ = day;
    guestTotals_ += delta;
    return true;
  }
  DayTotals rest = delta;
  do {
    DayTotals part;
    part.reviews = take16(rest.reviews);
    part.correct = take16(rest.correct);
    part.newItems = take16(rest.newItems);
    part.seconds = take16(rest.seconds);
    if (!writeRecord(day, part)) return false;
  } while (rest.reviews || rest.correct || rest.newItems || rest.seconds);
  return true;
}

bool DayLog::addChecked(DayNumber day, const DayTotals& delta) {
  if (uncertainWrite_) return false;
  if (add(day, delta)) return true;
  uncertainDelta_ = delta;
  uncertainDay_ = day;
  uncertainWrite_ = true;
  return false;
}

bool DayLog::totals(DayNumber day, DayTotals& out) {
  out = DayTotals();
  return forEach([&](DayNumber d, const DayTotals& t) {
    if (d == day) out += t;
  });
}

bool DayLog::range(DayNumber first, uint16_t count, DayTotals* out) {
  for (uint16_t i = 0; i < count; ++i) out[i] = DayTotals();
  return forEach([&](DayNumber d, const DayTotals& t) {
    if (d >= first && static_cast<uint32_t>(d - first) < count) out[d - first] += t;
  });
}

bool DayLog::studyDays(DayNumber first, uint16_t count, uint8_t* bits) {
  std::memset(bits, 0, (count + 7u) / 8u);
  return forEach([&](DayNumber d, const DayTotals& t) {
    if (!t.studied() || d < first || static_cast<uint32_t>(d - first) >= count) return;
    const uint32_t i = static_cast<uint32_t>(d - first);
    bits[i / 8] = static_cast<uint8_t>(bits[i / 8] | (1u << (i % 8)));
  });
}

bool DayLog::allTime(DayTotals& out, uint32_t* studyDays) {
  out = DayTotals();
  if (studyDays) *studyDays = 0;
  uint32_t lo = 0xFFFF, hi = 0;
  const bool ok = forEach([&](DayNumber d, const DayTotals& t) {
    out += t;
    if (!t.studied()) return;
    if (d < lo) lo = d;
    if (d > hi) hi = d;
  });
  if (!ok || !studyDays || lo > hi) return ok;
  // Distinct days, a 1,024-day window of bits at a time (one scan each;
  // nearly always one window).
  uint8_t bits[128];
  for (uint32_t first = lo; first <= hi; first += 1024) {
    const uint32_t span = hi - first + 1;
    const uint16_t count = static_cast<uint16_t>(span < 1024 ? span : 1024);
    if (!this->studyDays(static_cast<DayNumber>(first), count, bits)) return false;
    for (uint32_t i = 0; i < (count + 7u) / 8u; ++i) {
      for (uint8_t b = bits[i]; b; b = static_cast<uint8_t>(b & (b - 1))) ++*studyDays;
    }
  }
  return true;
}

}  // namespace tinta::core

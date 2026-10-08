#include "core/library/MarkLog.h"

#include <memory>
#include <new>

#include "core/srs/Bytes.h"

namespace tinta::core::library {

namespace {

constexpr uint8_t kMagic[4] = {'T', 'M', 'K', '1'};
constexpr uint8_t kAdd = 1;
constexpr uint8_t kRemove = 2;
// Records read per call while replaying.
constexpr uint32_t kChunk = 16;

void encode(uint8_t* out, uint32_t key, uint8_t op) {
  putU32(out, key);
  out[4] = op;
  out[5] = 0;
  putU16(out + 6, static_cast<uint16_t>(crc32(out, 6) & 0xFFFF));
}

}  // namespace

void MarkLog::open() {
  count_ = 0;
  records_ = 0;
  journalFailed_ = false;
  if (!store_.available()) return;
  if (mutationJournal_.recover && !mutationJournal_.recover(mutationJournal_.context)) {
    journalFailed_ = true;
    return;
  }
  const int32_t size = store_.size(file_);
  if (size < static_cast<int32_t>(kHeaderSize)) return;
  uint8_t head[kHeaderSize];
  if (store_.read(file_, 0, head, kHeaderSize) != static_cast<int32_t>(kHeaderSize)) return;
  for (uint8_t i = 0; i < 4; ++i) {
    if (head[i] != kMagic[i]) return;
  }
  const uint32_t total = (static_cast<uint32_t>(size) - kHeaderSize) / kRecordSize;
  uint8_t buf[kChunk * kRecordSize];
  for (uint32_t done = 0; done < total;) {
    const uint32_t n = total - done < kChunk ? total - done : kChunk;
    const int32_t got = store_.read(file_, kHeaderSize + done * kRecordSize, buf, n * kRecordSize);
    if (got != static_cast<int32_t>(n * kRecordSize)) break;
    for (uint32_t r = 0; r < n; ++r) {
      const uint8_t* rec = buf + r * kRecordSize;
      if (getU16(rec + 6) != static_cast<uint16_t>(crc32(rec, 6) & 0xFFFF)) continue;
      if (rec[4] == kAdd) insert(getU32(rec));
      if (rec[4] == kRemove) erase(getU32(rec));
    }
    done += n;
  }
  records_ = total;
}

bool MarkLog::contains(const uint32_t key) const {
  for (uint16_t i = 0; i < count_; ++i) {
    if (keys_[i] == key) return true;
  }
  return false;
}

void MarkLog::insert(const uint32_t key) {
  if (contains(key) || count_ >= kCapacity) return;
  keys_[count_++] = key;
}

void MarkLog::erase(const uint32_t key) {
  for (uint16_t i = 0; i < count_; ++i) {
    if (keys_[i] != key) continue;
    // Keep the order: the deck takes starred words first come, first served.
    for (uint16_t j = i; j + 1 < count_; ++j) keys_[j] = keys_[j + 1];
    --count_;
    return;
  }
}

bool MarkLog::add(const uint32_t key) {
  if (journalFailed_) return false;
  if (contains(key)) return true;
  if (count_ >= kCapacity || !persistMutation(key, true)) return false;
  insert(key);
  return append(key, kAdd);
}

bool MarkLog::remove(const uint32_t key) {
  if (journalFailed_) return false;
  if (!contains(key)) return true;
  if (!persistMutation(key, false)) return false;
  erase(key);
  return append(key, kRemove);
}

bool MarkLog::persistMutation(const uint32_t key, const bool enabled) {
  if (!store_.available() || !mutationJournal_.persist ||
      mutationJournal_.persist(mutationJournal_.context, key, enabled))
    return true;
  journalFailed_ = true;
  return false;
}

bool MarkLog::append(const uint32_t key, const uint8_t op) {
  if (!store_.available()) return false;
  if (records_ == 0 && store_.size(file_) < static_cast<int32_t>(kHeaderSize)) {
    if (!store_.replace(file_, kMagic, kHeaderSize)) return false;
  }
  uint8_t rec[kRecordSize];
  encode(rec, key, op);
  if (!store_.append(file_, rec, kRecordSize)) return false;
  ++records_;
  if (records_ > 2u * count_ + 32u) compact();
  return true;
}

void MarkLog::compact() {
  // One add per key, written whole and swapped in. On the heap for the
  // moment it takes: 772 bytes is too much for the stack, and compacting is
  // rare. Without the memory the log just stays longer.
  constexpr uint32_t kBufSize = kHeaderSize + kCapacity * kRecordSize;
  std::unique_ptr<uint8_t[]> owned(new (std::nothrow) uint8_t[kBufSize]);
  if (!owned) return;
  uint8_t* buf = owned.get();
  for (uint8_t i = 0; i < 4; ++i) buf[i] = kMagic[i];
  for (uint16_t i = 0; i < count_; ++i) encode(buf + kHeaderSize + i * kRecordSize, keys_[i], kAdd);
  const uint32_t len = kHeaderSize + count_ * kRecordSize;
  if (store_.replace(file_, buf, len)) records_ = count_;
}

}  // namespace tinta::core::library

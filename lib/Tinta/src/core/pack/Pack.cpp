#include "core/pack/Pack.h"

#include <cstring>

#include "core/pack/Fold.h"

namespace tinta::core::pack {

namespace {

struct CrcTable {
  uint32_t entry[256];
};

constexpr CrcTable makeCrcTable() {
  CrcTable t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    t.entry[i] = c;
  }
  return t;
}

// Byte-at-a-time CRC-32 (IEEE, as zlib): 1 KB of flash, and the full-pack
// check is rare enough that a faster slicing table is not worth more.
constexpr CrcTable kCrc = makeCrcTable();

uint32_t crcUpdate(uint32_t crc, const uint8_t* p, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) crc = kCrc.entry[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

bool littleEndianHost() {
  const uint16_t probe = 1;
  uint8_t first = 0;
  std::memcpy(&first, &probe, 1);
  return first == 1;
}

// First index in [0, n) where pred is false, for pred true on a prefix.
template <class Pred>
uint32_t partitionPoint(uint32_t n, Pred pred) {
  uint32_t lo = 0;
  while (n > 0) {
    const uint32_t half = n / 2;
    if (pred(lo + half)) {
      lo += half + 1;
      n -= half + 1;
    } else {
      n = half;
    }
  }
  return lo;
}

}  // namespace

PackStatus Pack::open(const uint8_t* data, uint32_t size) {
  memory_.reset(data, data == nullptr ? 0 : size);
  return open(memory_);
}

PackStatus Pack::open(PackSource& source) {
  close();
  source_ = &source;
  data_ = source.data();
  size_ = source.size();
  const PackStatus status = validate();
  if (status != PackStatus::Ok) {
    close();
    return status;
  }
  std::memcpy(locale_, header_.locale, sizeof header_.locale);
  locale_[sizeof header_.locale] = '\0';
  return PackStatus::Ok;
}

bool Pack::readAt(uint32_t offset, void* out, uint32_t len) const {
  if (data_ != nullptr) {
    if (offset > size_ || len > size_ - offset) return false;
    std::memcpy(out, data_ + offset, len);
    return true;
  }
  return source_ != nullptr && source_->read(offset, out, len);
}

// Fills header_ and sections_ as it goes (a local copy of the views would
// exceed the stack budget); open() clears them again on failure.
PackStatus Pack::validate() {
  if (!littleEndianHost()) return PackStatus::UnsupportedHost;
  const uint32_t size = size_;
  Header& h = header_;
  if (size < kHeaderSize || !readAt(0, &h, sizeof h)) return PackStatus::TooSmall;
  if (std::memcmp(h.magic, kMagic, sizeof kMagic) != 0) return PackStatus::BadMagic;
  if (h.formatMajor != kFormatMajor) return PackStatus::BadVersion;
  if (h.size != size || h.headerSize < kHeaderSize) return PackStatus::BadSize;
  // 64-bit sums: a corrupt offset near 4 GB must not wrap into range.
  const uint64_t dirEnd = uint64_t{h.directoryOffset} + uint64_t{h.sectionCount} * sizeof(DirEntry);
  if (h.directoryOffset % 4 != 0 || h.directoryOffset < h.headerSize || dirEnd > size) {
    return PackStatus::BadDirectory;
  }

  uint32_t seen = 0;  // bit per Section
  static_assert(kSectionCount <= 32);
  for (uint32_t d = 0; d < h.sectionCount; ++d) {
    DirEntry e;
    if (!readAt(h.directoryOffset + d * sizeof(DirEntry), &e, sizeof e)) return PackStatus::BadDirectory;
    if (e.offset % 4 != 0 || uint64_t{e.offset} + e.size > size) return PackStatus::BadSection;
    for (size_t s = 0; s < kSectionCount; ++s) {
      if (kSections[s].tag != e.tag) continue;
      if (seen & (1u << s)) return PackStatus::BadDirectory;
      seen |= 1u << s;
      sections_[s] = View{e.offset, e.size, e.count, 0};
    }
  }

  for (size_t s = 0; s < kSectionCount; ++s) {
    if (!(seen & (1u << s))) return PackStatus::MissingSection;
    View& v = sections_[s];
    const uint32_t recordSize = kSections[s].recordSize;
    if (recordSize == 0) continue;  // STRS
    // A later minor version may have grown the record, so the stride only has
    // to cover the fields this reader knows.
    v.stride = v.count == 0 ? 0 : v.size / v.count;
    if (uint64_t{v.count} * v.stride != v.size || (v.count != 0 && v.stride < recordSize)) {
      return PackStatus::BadSection;
    }
  }

  // Every string offset below STRS.size then finds a NUL inside the heap.
  const View& strs = sections_[static_cast<size_t>(Section::Strs)];
  uint8_t first = 1;
  uint8_t last = 1;
  if (strs.size == 0 || !readAt(strs.offset, &first, 1) || !readAt(strs.offset + strs.size - 1, &last, 1) ||
      first != 0 || last != 0) {
    return PackStatus::BadStrings;
  }
  return PackStatus::Ok;
}

void Pack::close() {
  source_ = nullptr;
  data_ = nullptr;
  size_ = 0;
  arenaUsed_ = 0;
  arenaPeak_ = 0;
  arenaOverflows_ = 0;
  memoCount_ = 0;
  memoNext_ = 0;
  header_ = Header{};
  std::memset(locale_, 0, sizeof locale_);
  std::memset(sections_, 0, sizeof sections_);
}

bool Pack::verifyCrc() const {
  if (source_ == nullptr) return false;
  static constexpr uint8_t kZero[4] = {};
  constexpr uint32_t kCrcAt = offsetof(Header, crc32);
  uint8_t chunk[128];
  uint32_t crc = 0xFFFFFFFFu;
  const auto run = [&](uint32_t from, uint32_t to) {
    while (from < to) {
      const uint32_t n = to - from < sizeof chunk ? to - from : static_cast<uint32_t>(sizeof chunk);
      if (!readAt(from, chunk, n)) return false;
      crc = crcUpdate(crc, chunk, n);
      from += n;
    }
    return true;
  };
  if (!run(0, kCrcAt)) return false;
  crc = crcUpdate(crc, kZero, sizeof kZero);
  if (!run(kCrcAt + 4, size_)) return false;
  return ~crc == header_.crc32;
}

void Pack::setArena(char* buffer, uint32_t size) {
  arena_ = buffer;
  arenaSize_ = buffer == nullptr ? 0 : size;
  arenaUsed_ = 0;
  memoCount_ = 0;
  memoNext_ = 0;
}

void Pack::beginPass() const {
  if (poison_ && arena_ != nullptr && arenaUsed_ > 0) std::memset(arena_, '#', arenaUsed_);
  arenaUsed_ = 0;
  memoCount_ = 0;
  memoNext_ = 0;
}

const char* Pack::str(uint32_t offset) const {
  const View& v = sections_[static_cast<size_t>(Section::Strs)];
  if (offset == 0 || offset >= v.size) return "";
  if (data_ != nullptr) return reinterpret_cast<const char*>(data_ + v.offset + offset);
  for (uint8_t i = 0; i < memoCount_; ++i) {
    if (memo_[i].offset == offset) return arena_ + memo_[i].at;
  }
  // Copy in chunks until the NUL; STRS ends in one (checked at open).
  const uint32_t at = arenaUsed_;
  uint32_t len = 0;
  for (;;) {
    const uint32_t room = arenaSize_ - (at + len);
    const uint32_t left = v.size - (offset + len);
    uint32_t n = room < 32 ? room : 32;
    if (n > left) n = left;
    if (n == 0 || !readAt(v.offset + offset + len, arena_ + at + len, n)) {
      ++arenaOverflows_;
      return "";
    }
    const void* nul = std::memchr(arena_ + at + len, 0, n);
    if (nul != nullptr) {
      len = static_cast<uint32_t>(static_cast<const char*>(nul) - (arena_ + at)) + 1;
      break;
    }
    len += n;
  }
  arenaUsed_ = at + len;
  if (arenaUsed_ > arenaPeak_) arenaPeak_ = arenaUsed_;
  memo_[memoNext_] = Memo{offset, at};
  memoNext_ = static_cast<uint8_t>((memoNext_ + 1) % kMemoSize);
  if (memoCount_ < kMemoSize) ++memoCount_;
  return arena_ + at;
}

uint32_t Pack::copyStr(uint32_t offset, char* out, uint32_t cap) const {
  if (cap == 0) return 0;
  out[0] = '\0';
  const View& v = sections_[static_cast<size_t>(Section::Strs)];
  if (offset == 0 || offset >= v.size) return 0;
  const uint32_t left = v.size - offset;
  const uint32_t n = cap - 1 < left ? cap - 1 : left;
  if (!readAt(v.offset + offset, out, n)) return 0;
  out[n] = '\0';
  return static_cast<uint32_t>(std::strlen(out));
}

uint32_t Pack::hashStr(uint32_t offset) const {
  uint32_t h = 2166136261u;
  const View& v = sections_[static_cast<size_t>(Section::Strs)];
  if (offset == 0 || offset >= v.size) return h;
  char chunk[32];
  for (uint32_t pos = offset; pos < v.size;) {
    const uint32_t n = v.size - pos < sizeof chunk ? v.size - pos : static_cast<uint32_t>(sizeof chunk);
    if (!readAt(v.offset + pos, chunk, n)) return h;
    for (uint32_t i = 0; i < n; ++i) {
      if (chunk[i] == '\0') return h;
      h ^= static_cast<uint8_t>(chunk[i]);
      h *= 16777619u;
    }
    pos += n;
  }
  return h;
}

bool Pack::visitStr(uint32_t offset, void* context, bool (*visit)(void*, const uint8_t*, uint32_t)) const {
  const View& v = sections_[static_cast<size_t>(Section::Strs)];
  if (!visit || offset == 0 || offset >= v.size) return false;
  uint8_t chunk[32];
  for (uint32_t pos = offset; pos < v.size;) {
    const uint32_t n = v.size - pos < sizeof chunk ? v.size - pos : static_cast<uint32_t>(sizeof chunk);
    if (!readAt(v.offset + pos, chunk, n)) return false;
    uint32_t length = 0;
    while (length < n && chunk[length] != 0) ++length;
    if (length && !visit(context, chunk, length)) return false;
    if (length < n) return true;
    pos += n;
  }
  return false;
}

bool Pack::readBytes(Section s, uint32_t index, uint32_t at, void* out, uint32_t len) const {
  const View& v = sections_[static_cast<size_t>(s)];
  // index < count bounds index * stride by the section size checked at open.
  if (index >= v.count || at > v.stride || len > v.stride - at) {
    std::memset(out, 0, len);
    return false;
  }
  if (!readAt(v.offset + index * v.stride + at, out, len)) {
    std::memset(out, 0, len);
    return false;
  }
  return true;
}

uint16_t Pack::lemmaExample(const Lemma& lem, uint16_t n) const {
  uint16_t sentence = kNone16;
  return readChild(Section::Lexs, lem.firstExample, lem.exampleCount, n, sentence) ? sentence : kNone16;
}

uint32_t Pack::itemCandidate(const Item& it, uint8_t n) const {
  uint32_t candidate = kNone32;
  return readChild(Section::Dist, it.firstCandidate, it.candidateCount, n, candidate) ? candidate : kNone32;
}

bool Pack::verbInfo(uint16_t table, VerbInfo& out) const {
  return readBytes(Section::Verb, table, offsetof(VerbTable, infinitive), &out, sizeof out);
}

const char* Pack::verbForm(uint16_t table, Tense tense, Person person) const {
  const auto t = static_cast<uint32_t>(tense);
  const auto p = static_cast<uint32_t>(person);
  if (t >= kTenseCount || p >= kPersonCount) return "";
  uint32_t offset = 0;
  const uint32_t at = offsetof(VerbTable, forms) + (t * kPersonCount + p) * sizeof(uint32_t);
  return readBytes(Section::Verb, table, at, &offset, sizeof offset) ? str(offset) : "";
}

const char* Pack::verbForm(uint16_t table, uint16_t tag) const {
  if (isVerbTag(tag)) return verbForm(table, tagTense(tag), tagPerson(tag));
  if (tag < kTagInfinitive || tag > kTagParticiple) return "";
  VerbInfo info;
  if (!verbInfo(table, info)) return "";
  if (tag == kTagInfinitive) return str(info.infinitive);
  return str(tag == kTagGerund ? info.gerund : info.participle);
}

TextSpan Pack::tokenText(const Sentence& s, const Token& t) const {
  const char* text = str(s.es);
  const auto len = static_cast<uint32_t>(std::strlen(text));
  if (t.start > len) return {text + len, 0};
  const uint32_t room = len - t.start;
  return {text + t.start, t.length < room ? t.length : room};
}

uint32_t Pack::keyAt(Section s, uint32_t index) const {
  uint32_t key = 0;  // every key record starts with its key offset
  readBytes(s, index, 0, &key, sizeof key);
  return key;
}

int Pack::compareKey(uint32_t offset, const char* key, uint32_t n, bool prefix) const {
  if (data_ != nullptr) {
    const char* k = str(offset);
    return prefix ? std::strncmp(k, key, n) : std::strcmp(k, key);
  }
  // n + 1 bytes decide either comparison.
  char k[kMaxQuery + 1];
  copyStr(offset, k, n + 2);
  return prefix ? std::strncmp(k, key, n) : std::strcmp(k, key);
}

KeyRange Pack::search(Section s, const char* query, bool prefix, KeyRange within) const {
  char key[kMaxQuery];
  const size_t n = foldKey(query, key, sizeof key);
  if (n >= sizeof key) return {};
  const auto compare = [&](uint32_t i) {
    return compareKey(keyAt(s, within.first + i), key, static_cast<uint32_t>(n), prefix);
  };
  const uint32_t total = count(s);
  if (within.first > total) return {};
  const uint32_t span = within.count < total - within.first ? within.count : total - within.first;
  const uint32_t first = partitionPoint(span, [&](uint32_t i) { return compare(i) < 0; });
  const uint32_t end = partitionPoint(span, [&](uint32_t i) { return compare(i) <= 0; });
  // An unsorted (corrupt) table can put end before first.
  return {within.first + first, end > first ? end - first : 0};
}

static_assert(offsetof(LemmaKey, key) == 0 && offsetof(EnglishKey, key) == 0 && offsetof(FormKey, key) == 0 &&
              offsetof(ItemUid, uid) == 0 && offsetof(Item, uid) == 0);

uint32_t Pack::uidAt(uint32_t index) const {
  uint32_t uid = 0;
  readBytes(Section::Item, index, offsetof(Item, uid), &uid, sizeof uid);
  return uid;
}

int32_t Pack::indexOfUid(uint32_t uid) const {
  const uint32_t total = count(Section::Iuid);
  if (total == 0) return -1;
  // Uids are handed out in order and few retire, so IUID is close to
  // uid -> position: start at the interpolated guess and gallop out to a
  // bracket. From the card that is one block instead of a dozen probes.
  const uint32_t lo = keyAt(Section::Iuid, 0);
  const uint32_t hi = keyAt(Section::Iuid, total - 1);
  uint32_t first = 0;
  uint32_t end = total;
  if (uid >= lo && uid <= hi && hi > lo) {
    const uint32_t guess = static_cast<uint32_t>(uint64_t{uid - lo} * (total - 1) / (hi - lo));
    if (keyAt(Section::Iuid, guess) < uid) {
      uint32_t step = 1;
      first = guess + 1;
      while (first + step < total && keyAt(Section::Iuid, first + step - 1) < uid) {
        first += step;
        step *= 2;
      }
      end = first + step < total ? first + step : total;
    } else {
      uint32_t step = 1;
      end = guess + 1;
      while (end > step && keyAt(Section::Iuid, end - step - 1) >= uid) {
        end -= step;
        step *= 2;
      }
      first = end > step ? end - step : 0;
    }
  }
  const uint32_t i =
      first + partitionPoint(end - first, [&](uint32_t j) { return keyAt(Section::Iuid, first + j) < uid; });
  ItemUid entry;
  // The uid check also rejects an index that a corrupt IUID points elsewhere.
  if (!read(Section::Iuid, i, entry) || entry.uid != uid || entry.index >= itemCount() || uidAt(entry.index) != uid) {
    return -1;
  }
  return entry.index;
}

ItemKind Pack::kindAt(uint32_t index) const {
  Item it;
  item(index, it);
  return it.kind;
}

uint16_t Pack::lessonAt(uint32_t index) const {
  Item it;
  return item(index, it) ? it.lesson : kNone16;
}

int32_t Pack::prerequisiteOf(uint32_t index) const {
  Item it;
  if (!item(index, it) || it.prereq == kNone16 || it.prereq >= itemCount()) return -1;
  return it.prereq;
}

}  // namespace tinta::core::pack

#include "core/profile/Profile.h"

#include <cstring>

#include "core/srs/Bytes.h"

namespace tinta::core {

namespace {

constexpr uint8_t kMagic[4] = {'T', 'P', 'R', 'F'};
constexpr uint32_t kHeader = 8;

// Reads payload fields, falling back to the default when the field is past
// the end of an older payload or outside its range.
class FieldReader {
 public:
  FieldReader(const uint8_t* payload, uint32_t length) : p_(payload), length_(length) {}

  bool fixedUp() const { return fixedUp_; }

  uint16_t u16(uint32_t at, uint16_t def, uint16_t lo, uint16_t hi) {
    if (at + 2 > length_) return def;
    return check(getU16(p_ + at), def, lo, hi);
  }
  uint8_t u8(uint32_t at, uint8_t def, uint8_t lo, uint8_t hi) {
    if (at + 1 > length_) return def;
    return static_cast<uint8_t>(check(p_[at], def, lo, hi));
  }
  int16_t s16(uint32_t at, int16_t def, int16_t lo, int16_t hi) {
    if (at + 2 > length_) return def;
    const int16_t v = static_cast<int16_t>(getU16(p_ + at));
    if (v < lo || v > hi) {
      fixedUp_ = true;
      return def;
    }
    return v;
  }

 private:
  uint16_t check(uint16_t v, uint16_t def, uint16_t lo, uint16_t hi) {
    if (v < lo || v > hi) {
      fixedUp_ = true;
      return def;
    }
    return v;
  }

  const uint8_t* p_;
  uint32_t length_;
  bool fixedUp_ = false;
};

}  // namespace

void Profile::encode(uint8_t out[kEncodedSize]) const {
  std::memset(out, 0, kEncodedSize);
  std::memcpy(out, kMagic, 4);
  putU16(out + 4, kVersion);
  putU16(out + 6, static_cast<uint16_t>(kPayloadSize));
  uint8_t* p = out + kHeader;
  putU16(p + 0, newPerDay);
  putU16(p + 2, reviewCap);
  putU16(p + 4, retentionPermille);
  putU16(p + 6, maxInterval);
  putU16(p + 8, sessionSize);
  p[10] = static_cast<uint8_t>(textSize);
  p[11] = static_cast<uint8_t>(uiLanguage);
  p[12] = showVulgar ? 1 : 0;
  p[13] = fullRefreshEvery;
  putU16(p + 14, sleepTimeoutSeconds);
  p[16] = static_cast<uint8_t>(sleepScreen);
  p[17] = rolloverHour;
  putU16(p + 18, static_cast<uint16_t>(utcOffsetMinutes));
  putU16(p + 20, lastConfirmedDay);
  putU16(p + 22, currentLesson);
  putU16(p + 24, unlockedThrough);
  p[26] = frontlightBrightness;
  p[27] = frontlightWarmth;
  p[28] = typedAnswers ? 1 : 0;
  putU16(p + 29, sleepCount);
  putU32(out + kHeader + kPayloadSize, crc32(out, kHeader + kPayloadSize));
}

Profile::LoadResult Profile::decode(const uint8_t* in, uint32_t length) {
  *this = Profile();
  if (length < kHeader + 4 || std::memcmp(in, kMagic, 4) != 0) return LoadResult::Corrupt;
  const uint32_t payload = getU16(in + 6);
  if (kHeader + payload + 4 > length) return LoadResult::Corrupt;
  if (getU32(in + kHeader + payload) != crc32(in, kHeader + payload)) return LoadResult::Corrupt;
  return decodeFields(in, payload);
}

Profile::LoadResult Profile::decodeFields(const uint8_t* in, uint32_t available) {
  const uint16_t version = getU16(in + 4);
  const uint32_t payload = getU16(in + 6);
  if (version == 0) return LoadResult::Corrupt;
  if (available > payload) available = payload;

  const Profile d;
  FieldReader r(in + kHeader, available);
  newPerDay = r.u16(0, d.newPerDay, 0, 200);
  reviewCap = r.u16(2, d.reviewCap, 0, 9999);
  retentionPermille = r.u16(4, d.retentionPermille, 700, 970);
  maxInterval = r.u16(6, d.maxInterval, 1, 36500);
  sessionSize = r.u16(8, d.sessionSize, 5, 500);
  textSize = static_cast<TextSize>(r.u8(10, static_cast<uint8_t>(d.textSize), 0, 2));
  uiLanguage = static_cast<UiLanguage>(r.u8(11, static_cast<uint8_t>(d.uiLanguage), 0, 2));
  showVulgar = r.u8(12, 0, 0, 1) != 0;
  fullRefreshEvery = r.u8(13, d.fullRefreshEvery, 1, 50);
  sleepTimeoutSeconds = r.u16(14, d.sleepTimeoutSeconds, 30, 3600);
  sleepScreen = static_cast<SleepScreen>(r.u8(16, static_cast<uint8_t>(d.sleepScreen), 0, 2));
  rolloverHour = r.u8(17, d.rolloverHour, 0, 23);
  utcOffsetMinutes = r.s16(18, d.utcOffsetMinutes, -720, 840);
  lastConfirmedDay = r.u16(20, d.lastConfirmedDay, 0, 0xFFFF);
  currentLesson = r.u16(22, d.currentLesson, 0, 0xFFFF);
  unlockedThrough = r.u16(24, d.unlockedThrough, 0, 0xFFFF);
  frontlightBrightness = r.u8(26, d.frontlightBrightness, 0, 100);
  frontlightWarmth = r.u8(27, d.frontlightWarmth, 0, 100);
  typedAnswers = r.u8(28, 0, 0, 1) != 0;
  sleepCount = r.u16(29, 0, 0, 0xFFFF);

  if (version != kVersion || payload != kPayloadSize || r.fixedUp()) return LoadResult::Upgraded;
  return LoadResult::Loaded;
}

Profile::LoadResult Profile::load(StateStore& store) {
  *this = Profile();
  if (!store.available()) return LoadResult::Defaults;
  const int32_t size = store.size(kFile);
  if (size < 0) return LoadResult::Defaults;

  // Streamed, so that a newer firmware's longer profile still checks out.
  uint8_t head[kHeader + kPayloadSize];
  if (size < static_cast<int32_t>(kHeader + 4) ||
      store.read(kFile, 0, head, kHeader) != static_cast<int32_t>(kHeader) || std::memcmp(head, kMagic, 4) != 0) {
    return LoadResult::Corrupt;
  }
  const uint32_t covered = kHeader + getU16(head + 6);
  if (static_cast<uint32_t>(size) < covered + 4) return LoadResult::Corrupt;

  uint32_t crc = 0;
  uint8_t chunk[64];
  for (uint32_t at = 0; at < covered; at += sizeof chunk) {
    const uint32_t n = covered - at < sizeof chunk ? covered - at : sizeof chunk;
    if (store.read(kFile, at, chunk, n) != static_cast<int32_t>(n)) return LoadResult::Corrupt;
    crc = crc32Update(crc, chunk, n);
    for (uint32_t i = 0; i < n && at + i < sizeof head; ++i) head[at + i] = chunk[i];
  }
  if (store.read(kFile, covered, chunk, 4) != 4 || getU32(chunk) != crc) return LoadResult::Corrupt;
  const uint32_t available = covered - kHeader < kPayloadSize ? covered - kHeader : kPayloadSize;
  return decodeFields(head, available);
}

bool Profile::save(StateStore& store) const {
  if (!store.available()) return false;
  uint8_t buffer[kEncodedSize];
  encode(buffer);
  return store.replace(kFile, buffer, kEncodedSize);
}

}  // namespace tinta::core

// modules: profile
//
// profile.bin: defaults, round trip, upgrades in both directions, range
// checks, corruption and a power cut during replace().

#include "core/profile/Profile.h"

#include <cstring>
#include <vector>

#include "check.h"
#include "core/srs/Bytes.h"
#include "fakes.h"

using namespace tinta::core;
using namespace tinta_test;
using LoadResult = Profile::LoadResult;

namespace {

bool same(const Profile& a, const Profile& b) {
  uint8_t x[Profile::kEncodedSize], y[Profile::kEncodedSize];
  a.encode(x);
  b.encode(y);
  return std::memcmp(x, y, sizeof x) == 0;
}

Profile custom() {
  Profile p;
  p.newPerDay = 15;
  p.reviewCap = 250;
  p.retentionPermille = 920;
  p.maxInterval = 1000;
  p.sessionSize = 60;
  p.textSize = TextSize::Large;
  p.uiLanguage = UiLanguage::Auto;
  p.showVulgar = true;
  p.fullRefreshEvery = 12;
  p.sleepTimeoutSeconds = 600;
  p.sleepScreen = SleepScreen::Rotating;
  p.rolloverHour = 3;
  p.utcOffsetMinutes = -360;
  p.lastConfirmedDay = 1006;
  p.currentLesson = 7;
  p.unlockedThrough = 8;
  p.frontlightBrightness = 30;
  p.frontlightWarmth = 80;
  p.typedAnswers = true;
  p.sleepCount = 4321;
  return p;
}

// A file with an arbitrary version and payload, correctly checksummed.
std::vector<uint8_t> file(uint16_t version, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> f(8 + payload.size() + 4);
  std::memcpy(f.data(), "TPRF", 4);
  putU16(f.data() + 4, version);
  putU16(f.data() + 6, static_cast<uint16_t>(payload.size()));
  std::memcpy(f.data() + 8, payload.data(), payload.size());
  putU32(f.data() + 8 + payload.size(), crc32(f.data(), 8 + payload.size()));
  return f;
}

std::vector<uint8_t> payloadOf(const Profile& p) {
  uint8_t bytes[Profile::kEncodedSize];
  p.encode(bytes);
  return std::vector<uint8_t>(bytes + 8, bytes + 8 + Profile::kPayloadSize);
}

void testDefaultsAndRoundTrip() {
  MemStore store;
  Profile p = custom();
  CHECK(p.load(store) == LoadResult::Defaults);
  CHECK(same(p, Profile()));
  CHECK_EQ(p.newPerDay, 10);
  CHECK_EQ(p.reviewCap, 100);
  CHECK_NEAR(p.desiredRetention(), 0.90, 1e-6);
  CHECK_EQ(p.maxInterval, 365);
  CHECK_EQ(p.fullRefreshEvery, 8);
  CHECK_EQ(p.sleepTimeoutSeconds, 180);
  CHECK_EQ(p.rolloverHour, 4);
  CHECK(p.uiLanguage == UiLanguage::English);

  CHECK(custom().save(store));
  CHECK_EQ(store.size(Profile::kFile), Profile::kEncodedSize);
  Profile q;
  CHECK(q.load(store) == LoadResult::Loaded);
  CHECK(same(q, custom()));
  CHECK_EQ(q.utcOffsetMinutes, -360);
  CHECK(q.showVulgar);
  CHECK(q.typedAnswers);
  CHECK_EQ(q.sleepCount, 4321);
  CHECK(!Profile().typedAnswers);

  // Layout spot check: little-endian, fixed offsets.
  const std::vector<uint8_t>& f = store.files[Profile::kFile];
  CHECK(std::memcmp(f.data(), "TPRF", 4) == 0);
  CHECK_EQ(getU16(f.data() + 4), 1);
  CHECK_EQ(getU16(f.data() + 6), Profile::kPayloadSize);
  CHECK_EQ(getU16(f.data() + 8 + 2), 250);
  CHECK_EQ(static_cast<int16_t>(getU16(f.data() + 8 + 18)), -360);
}

void testUpgrades() {
  MemStore store;
  // An older, shorter payload: the fields it has, defaults for the rest.
  std::vector<uint8_t> older = payloadOf(custom());
  older.resize(14);
  store.files[Profile::kFile] = file(1, older);
  Profile p;
  CHECK(p.load(store) == LoadResult::Upgraded);
  CHECK_EQ(p.newPerDay, 15);
  CHECK_EQ(p.fullRefreshEvery, 12);
  CHECK_EQ(p.sleepTimeoutSeconds, 180);  // past the old end
  CHECK_EQ(p.lastConfirmedDay, 0);
  CHECK_EQ(p.frontlightWarmth, 50);
  // Saving rewrites it in the current layout.
  CHECK(p.save(store));
  CHECK(p.load(store) == LoadResult::Loaded);
  CHECK_EQ(p.newPerDay, 15);

  // A newer firmware's longer payload, even a long one: known fields load.
  for (size_t extra : {12u, 400u}) {
    std::vector<uint8_t> newer = payloadOf(custom());
    newer.resize(newer.size() + extra, 0x77);
    store.files[Profile::kFile] = file(3, newer);
    CHECK(p.load(store) == LoadResult::Upgraded);
    CHECK(same(p, custom()));
  }

  // Out-of-range fields take their defaults; the rest are kept.
  std::vector<uint8_t> bad = payloadOf(custom());
  putU16(bad.data() + 4, 500);  // retention 0.5
  bad[10] = 9;                  // text size
  bad[17] = 30;                 // rollover hour
  putU16(bad.data() + 18, static_cast<uint16_t>(-2000));
  store.files[Profile::kFile] = file(1, bad);
  CHECK(p.load(store) == LoadResult::Upgraded);
  CHECK_EQ(p.retentionPermille, 900);
  CHECK(p.textSize == TextSize::Medium);
  CHECK_EQ(p.rolloverHour, 4);
  CHECK_EQ(p.utcOffsetMinutes, 0);
  CHECK_EQ(p.reviewCap, 250);
  CHECK_EQ(p.lastConfirmedDay, 1006);

  // The in-memory decoder agrees with load().
  const std::vector<uint8_t> good = file(1, payloadOf(custom()));
  CHECK(p.decode(good.data(), static_cast<uint32_t>(good.size())) == LoadResult::Loaded);
  CHECK(same(p, custom()));
}

void testCorrupt() {
  MemStore store;
  custom().save(store);
  const std::vector<uint8_t> good = store.files[Profile::kFile];
  Profile p;

  std::vector<uint8_t> f = good;
  f[12] ^= 0x01;  // payload bit flip
  store.files[Profile::kFile] = f;
  CHECK(p.load(store) == LoadResult::Corrupt);
  CHECK(same(p, Profile()));

  f = good;
  f.resize(f.size() - 3);  // truncated
  store.files[Profile::kFile] = f;
  CHECK(p.load(store) == LoadResult::Corrupt);

  f = good;
  f[0] = 'X';  // not a profile
  store.files[Profile::kFile] = f;
  CHECK(p.load(store) == LoadResult::Corrupt);

  store.files[Profile::kFile] = std::vector<uint8_t>(3, 0);
  CHECK(p.load(store) == LoadResult::Corrupt);

  std::vector<uint8_t> zero = file(0, payloadOf(custom()));  // version 0 never existed
  store.files[Profile::kFile] = zero;
  CHECK(p.load(store) == LoadResult::Corrupt);
  CHECK(same(p, Profile()));
}

void testPowerCut() {
  MemStore store;
  custom().save(store);
  Profile changed = custom();
  changed.newPerDay = 3;
  for (MemStore::Tear tear : {MemStore::Tear::Nothing, MemStore::Tear::Prefix, MemStore::Tear::Garbage}) {
    store.cutAt(store.calls, tear);
    CHECK(!changed.save(store));
    store.powerOn();
    Profile p;
    CHECK(p.load(store) == LoadResult::Loaded);
    CHECK(same(p, custom()));  // the old file, whole
  }
  CHECK(changed.save(store));
  Profile p;
  CHECK(p.load(store) == LoadResult::Loaded);
  CHECK_EQ(p.newPerDay, 3);

  MemStore guest;
  guest.present = false;
  CHECK(!custom().save(guest));
  CHECK(p.load(guest) == LoadResult::Defaults);
}

// The card stops answering at each size() or read() call of load(): the
// caller then gets defaults, and save() must not replace the real profile.
void testCardDropsOut() {
  MemStore base;
  custom().save(base);
  const auto before = base.files;
  int drops = 0;
  for (int which = 0; which < 2; ++which) {
    for (int k = 0; k < 6; ++k) {
      MemStore store = base;
      if (which == 0) {
        store.dropAtSize(store.sizeCalls + k);
      } else {
        store.dropAtRead(store.readCalls + k);
      }
      Profile p;
      const LoadResult r = p.load(store);
      if (!store.gone()) {
        CHECK(r == LoadResult::Loaded);
        continue;
      }
      ++drops;
      CHECK(r == LoadResult::Defaults || r == LoadResult::Corrupt);
      CHECK(same(p, Profile()));
      CHECK(!p.save(store));  // a shell saving the defaults it got
      CHECK_EQ(store.attemptsWhileGone, 0);
      CHECK(store.files == before);
      store.powerOn();
      CHECK(p.load(store) == LoadResult::Loaded);
      CHECK(same(p, custom()));
    }
  }
  CHECK(drops >= 3);
}

}  // namespace

int main() {
  testDefaultsAndRoundTrip();
  testUpgrades();
  testCorrupt();
  testPowerCut();
  testCardDropsOut();
  return tinta_test::result();
}

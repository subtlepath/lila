// modules: usage
//
// The usage log (PLAN.md 8.6): record and chunk encoding, the buffer and its
// dropped-record count, guest mode and the setting, failed and torn writes,
// file rotation. A sample session written here is also the fixture
// tools/tests/test_usage_report.py decodes (the two must agree byte for byte).

#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "core/srs/Bytes.h"
#include "core/usage/UsageLog.h"
#include "fakes.h"

using namespace tinta::core;
using namespace tinta::core::usage;
using namespace tinta_test;

namespace {

uint32_t gUptime = 0;
uint32_t uptime() { return gUptime; }

const char* const kFile1 = "usage-0001.log";
const char* const kFixture = "fixtures/usage-sample/tinta/usage-0001.log";
const char* const kSampleOut = "build/usage-sample/tinta/usage-0001.log";

struct Record {
  uint8_t type;
  uint32_t uptime;
  std::vector<uint8_t> fields;
};

struct Chunk {
  uint32_t wall;
  uint32_t uptime;
  uint8_t flags;
  std::vector<Record> records;
};

// A decoder written from the header comment, independent of the encoder:
// chunks found by their magic and checked, records walked by their size.
std::vector<Chunk> decode(const std::vector<uint8_t>& file, int* skipped = nullptr) {
  std::vector<Chunk> out;
  size_t at = 0;
  int bad = 0;
  while (at + UsageLog::kChunkHeader <= file.size()) {
    if (file[at] != 'T' || file[at + 1] != 'U') {
      ++at;
      continue;
    }
    const uint16_t bytes = getU16(&file[at + 2]);
    const uint16_t check = getU16(&file[at + 4]);
    const size_t body = at + UsageLog::kChunkHeader;
    if (body + bytes > file.size() || static_cast<uint16_t>(crc32(&file[body], bytes)) != check) {
      ++bad;
      ++at;
      continue;
    }
    Chunk c;
    c.flags = file[at + 6];
    c.wall = getU32(&file[at + 8]);
    c.uptime = getU32(&file[at + 12]);
    size_t r = body;
    while (r + UsageLog::kRecordHeader <= body + bytes) {
      Record rec;
      rec.type = file[r];
      const uint8_t size = file[r + 1];
      rec.uptime = getU32(&file[r + 2]);
      rec.fields.assign(file.begin() + static_cast<long>(r + UsageLog::kRecordHeader),
                        file.begin() + static_cast<long>(r + UsageLog::kRecordHeader + size));
      c.records.push_back(rec);
      r += UsageLog::kRecordHeader + size;
    }
    CHECK_EQ(r, body + bytes);
    out.push_back(c);
    at = body + bytes;
  }
  if (skipped) *skipped = bad;
  return out;
}

std::vector<Record> records(MemStore& store, const char* file = kFile1) {
  std::vector<Record> out;
  for (const Chunk& c : decode(store.files[file])) out.insert(out.end(), c.records.begin(), c.records.end());
  return out;
}

std::string text(const std::vector<uint8_t>& f, size_t at) {
  return std::string(f.begin() + static_cast<long>(at + 1), f.begin() + static_cast<long>(at + 1 + f[at]));
}

BootInfo bootInfo() {
  BootInfo info;
  info.reason = BootReason::WakeKey;
  info.device = "X4CLASSIC";
  info.build = "x4-classic";
  info.version = "0.7.0-draft";
  info.packEdition = 1;
  info.packMajor = 1;
  info.packMinor = 1;
  info.packCrc = 0xacd59ceb;
  info.packBuildTime = 1790000000;
  return info;
}

void testEncoding() {
  MemStore store;
  FakeClock clock(1000);
  gUptime = 1200;
  UsageLog log(store, clock, uptime);
  log.open(true);
  log.boot(bootInfo());
  gUptime = 1500;
  log.screen(1, 1, 3);
  const char* options[] = {"house", "table", "dog"};
  gUptime = 4000;
  log.itemShown(42, 4, 0, 3, 2, options, 3, 0);
  gUptime = 6500;
  log.answer(42, 4, 1, Correct::Wrong, 1, 2500, 0);
  CHECK(log.flush());
  CHECK_EQ(log.buffered(), 0);

  const std::vector<Chunk> chunks = decode(store.files[kFile1]);
  CHECK_EQ(chunks.size(), 1);
  if (chunks.size() != 1) return;
  const Chunk& c = chunks[0];
  CHECK_EQ(c.flags, 1);
  CHECK_EQ(c.wall, clock.nowSeconds());
  CHECK_EQ(c.uptime, 6500);
  CHECK_EQ(c.records.size(), 4);
  if (c.records.size() != 4) return;

  const Record& boot = c.records[0];
  CHECK_EQ(boot.type, static_cast<uint8_t>(Type::Boot));
  CHECK_EQ(boot.uptime, 1200);
  CHECK_EQ(boot.fields[0], static_cast<uint8_t>(BootReason::WakeKey));
  CHECK(text(boot.fields, 1) == "X4CLASSIC");
  CHECK(text(boot.fields, 11) == "x4-classic");
  CHECK(text(boot.fields, 22) == "0.7.0-draft");
  CHECK_EQ(getU32(&boot.fields[34]), 1);  // edition
  CHECK_EQ(getU32(&boot.fields[40]), 0xacd59ceb);
  CHECK_EQ(getU16(&boot.fields[48]), 1000);  // study day

  const Record& shown = c.records[2];
  CHECK_EQ(shown.type, static_cast<uint8_t>(Type::ItemShown));
  CHECK_EQ(getU32(&shown.fields[0]), 42);
  CHECK_EQ(shown.fields[4], 4);           // format
  CHECK_EQ(getU16(&shown.fields[6]), 3);  // lesson
  CHECK_EQ(shown.fields[9], 0);           // answer index
  CHECK_EQ(shown.fields[10], 3);          // options
  CHECK(text(shown.fields, 11) == "house");
  CHECK(text(shown.fields, 17) == "table");
  CHECK(text(shown.fields, 23) == "dog");

  const Record& answer = c.records[3];
  CHECK_EQ(answer.type, static_cast<uint8_t>(Type::Answer));
  CHECK_EQ(answer.fields[5], 1);  // chosen
  CHECK_EQ(answer.fields[6], static_cast<uint8_t>(Correct::Wrong));
  CHECK_EQ(getU32(&answer.fields[8]), 2500);
  CHECK_EQ(answer.fields[13], 0);  // no typed text

  // Records accumulate in the same file, one chunk per flush.
  log.undo(42);
  CHECK(log.flush());
  CHECK(log.flush());  // nothing buffered: nothing written
  CHECK_EQ(decode(store.files[kFile1]).size(), 2);
}

void testTextCaps() {
  MemStore store;
  FakeClock clock;
  UsageLog log(store, clock, uptime);
  log.open(true);
  // 23 ASCII bytes then "ñ" (2 bytes) straddles the 24-byte cap: cut before it.
  log.gloss(7, Source::Reader, "abcdefghijklmnopqrstuvwñañ");
  log.search(SearchMode::Spanish, "", 0);
  std::string longTyped(100, 'x');
  log.answer(1, 10, 0xFF, Correct::Right, 3, 900, 0, longTyped.c_str());
  CHECK(log.flush());
  const std::vector<Record> r = records(store);
  CHECK_EQ(r.size(), 3);
  if (r.size() != 3) return;
  CHECK(text(r[0].fields, 3) == "abcdefghijklmnopqrstuvw");
  CHECK_EQ(r[1].fields[3], 0);
  CHECK_EQ(r[2].fields[13], UsageLog::kTypedCap);
}

void testGuestAndSetting() {
  MemStore guest;
  guest.present = false;
  FakeClock clock;
  UsageLog log(guest, clock, uptime);
  log.open(true);
  log.boot(bootInfo());
  log.undo(1);
  CHECK_EQ(log.buffered(), 0);
  CHECK(log.flush());
  CHECK(guest.files.empty());
  CHECK_EQ(log.dropped(), 0);

  MemStore store;
  UsageLog off(store, clock, uptime);
  off.open(false);
  off.boot(bootInfo());
  off.undo(1);
  CHECK_EQ(off.buffered(), 0);
  off.setEnabled(true);  // says so, and repeats the boot record
  off.undo(2);
  off.setEnabled(false);
  off.undo(3);
  CHECK(off.flush());
  const std::vector<Record> r = records(store);
  CHECK_EQ(r.size(), 4);
  if (r.size() != 4) return;
  CHECK_EQ(r[0].type, static_cast<uint8_t>(Type::LogState));
  CHECK_EQ(r[0].fields[0], 1);
  CHECK_EQ(r[1].type, static_cast<uint8_t>(Type::Boot));
  CHECK_EQ(r[1].fields[0], static_cast<uint8_t>(BootReason::Enabled));
  CHECK_EQ(getU32(&r[2].fields[0]), 2);
  CHECK_EQ(r[3].type, static_cast<uint8_t>(Type::LogState));
  CHECK_EQ(r[3].fields[0], 0);
}

void testFullBufferAndDrops() {
  MemStore store;
  FakeClock clock;
  UsageLog log(store, clock, uptime);
  log.open(true);
  // Undo records are 10 bytes: fill the buffer without flushing.
  bool askedToFlush = false;
  for (int i = 0; i < 200; ++i) {
    log.undo(static_cast<uint32_t>(i));
    askedToFlush = askedToFlush || log.needsFlush();
  }
  CHECK(askedToFlush);
  CHECK(log.dropped() > 0);
  CHECK(log.flush());
  log.undo(999);
  CHECK(log.flush());
  // Every record is either in the file or counted by a Dropped record that
  // stands where the loss happened; the next chunk opens with the rest.
  const std::vector<Chunk> chunks = decode(store.files[kFile1]);
  CHECK_EQ(chunks.size(), 2);
  if (chunks.size() != 2) return;
  uint32_t undos = 0;
  uint32_t droppedCounted = 0;
  for (const Chunk& c : chunks) {
    for (const Record& r : c.records) {
      if (r.type == static_cast<uint8_t>(Type::Undo)) ++undos;
      if (r.type == static_cast<uint8_t>(Type::Dropped)) droppedCounted += getU32(&r.fields[0]);
    }
  }
  CHECK_EQ(undos + droppedCounted, 201);
  CHECK_EQ(droppedCounted, log.dropped());
  CHECK_EQ(chunks[1].records[0].type, static_cast<uint8_t>(Type::Dropped));
  CHECK_EQ(getU32(&chunks[1].records[1].fields[0]), 999);

  // A failed write loses its records, counted the same way.
  MemStore failing;
  UsageLog flaky(failing, clock, uptime);
  flaky.open(true);
  flaky.undo(1);
  flaky.undo(2);
  failing.failFrom(failing.calls);
  CHECK(!flaky.flush());
  CHECK_EQ(flaky.dropped(), 2);
  failing.powerOn();
  flaky.undo(3);
  CHECK(flaky.flush());
  const std::vector<Record> r = records(failing);
  CHECK_EQ(r.size(), 2);
  if (r.size() == 2) {
    CHECK_EQ(r[0].type, static_cast<uint8_t>(Type::Dropped));
    CHECK_EQ(getU32(&r[0].fields[0]), 2);
  }
}

void testTornChunk() {
  MemStore store;
  FakeClock clock;
  UsageLog log(store, clock, uptime);
  log.open(true);
  log.boot(bootInfo());
  CHECK(log.flush());
  log.undo(1);
  log.undo(2);
  store.cutAt(store.calls, MemStore::Tear::Prefix);  // power cut halfway through
  CHECK(!log.flush());
  store.powerOn();

  // The next boot appends after the torn chunk.
  UsageLog next(store, clock, uptime);
  next.open(true);
  next.boot(bootInfo());
  next.undo(3);
  CHECK(next.flush());
  int skipped = 0;
  const std::vector<Chunk> chunks = decode(store.files[kFile1], &skipped);
  CHECK_EQ(chunks.size(), 2);
  CHECK(skipped > 0);
  if (chunks.size() == 2) {
    CHECK_EQ(chunks[1].records.size(), 2);
    CHECK_EQ(getU32(&chunks[1].records[1].fields[0]), 3);
  }
}

void testRotation() {
  MemStore store;
  FakeClock clock;
  UsageLog log(store, clock, uptime);
  log.open(true);
  log.boot(bootInfo());
  const uint32_t cap = UsageLog::kFileCap;
  // About a kilobyte per flush, past the cap of kKeepFiles + 2 files.
  const uint32_t flushes = (cap / 700) * (UsageLog::kKeepFiles + 2);
  for (uint32_t n = 0; n < flushes; ++n) {
    while (!log.needsFlush()) log.undo(n);
    CHECK(log.flush());
  }
  const uint16_t last = log.fileNumber();
  CHECK(last >= UsageLog::kKeepFiles + 2);
  char name[24];
  int kept = 0;
  for (uint16_t seq = 1; seq <= last; ++seq) {
    std::snprintf(name, sizeof name, "usage-%04u.log", seq);
    auto it = store.files.find(name);
    if (it == store.files.end()) continue;
    ++kept;
    CHECK(it->second.size() <= cap);
    // Every file begins with this boot's record, as FileStart after the first.
    const std::vector<Chunk> chunks = decode(it->second);
    CHECK(!chunks.empty() && !chunks[0].records.empty());
    if (chunks.empty() || chunks[0].records.empty()) continue;
    CHECK_EQ(chunks[0].records[0].type, static_cast<uint8_t>(Type::Boot));
    if (seq > 1) CHECK_EQ(chunks[0].records[0].fields[0], static_cast<uint8_t>(BootReason::FileStart));
  }
  CHECK_EQ(kept, UsageLog::kKeepFiles);
  std::snprintf(name, sizeof name, "usage-%04u.log", last - UsageLog::kKeepFiles);
  CHECK(store.files.find(name) == store.files.end());

  // The next boot carries on in the newest file.
  UsageLog next(store, clock, uptime);
  next.open(true);
  CHECK_EQ(next.fileNumber(), last);
}

// A deterministic short session: the fixture for the Python decoder.
void sampleSession(MemStore& store) {
  FakeClock clock(1007);  // 2026-10-04 is day 1007 (2024-01-01 is day 0)
  gUptime = 900;
  UsageLog log(store, clock, uptime);
  log.open(true);
  log.boot(bootInfo());
  log.screen(1, 1, 3);  // Home, restored
  gUptime = 5000;
  log.input(Input::Confirm, Outcome::Handled, 1);
  log.screen(16, 2, 0);  // Session
  log.sessionStart(SessionKind::Today, 0, 3, 2, 1);
  const char* opts1[] = {"house", "table", "dog", "cat"};
  log.itemShown(1, 4, 0, 1, 2, opts1, 4, 0);
  gUptime = 9000;
  log.input(Input::Right, Outcome::Handled, 16);
  log.answer(1, 4, 2, Correct::Wrong, 1, 3400, 0);
  gUptime = 10000;
  log.input(Input::Up, Outcome::Ignored, 16);
  gUptime = 12000;
  log.input(Input::Confirm, Outcome::Queued, 16);
  const char* opts2[] = {"la", "el"};
  log.itemShown(2, 7, 4, 1, 0, opts2, 2, 1);
  gUptime = 14000;
  log.answer(2, 7, 1, Correct::Right, 3, 2000, 0);
  CHECK(log.flush());  // after the journal write, as the app does
  gUptime = 15000;
  log.itemShown(3, 10, 1, 2, 1, nullptr, 0, 0xFF);
  gUptime = 21000;
  log.answer(3, 10, 0xFF, Correct::Near, 2, 6000, 1, "mésa");
  log.sessionEnd(SessionEndHow::Finished, 3, 2, 16);
  CHECK(log.flush());
  gUptime = 30000;
  log.screen(23, 2, 0);  // a reading
  log.storyOpen(0xBEEF, 4);
  log.storyPage(0xBEEF, 1, 4);
  log.gloss(120, Source::Reader, "despertarse");
  log.gloss(120, Source::Reader, "despertarse");
  log.star(77, true, "despertarse");
  log.sentenceEnglish(0xBEEF, 9, Source::Reader);
  log.quizAnswer(0xBEEF, 0, 2, false);
  log.storyDone(0xBEEF, 1, 2);
  log.search(SearchMode::Spanish, "fui", 3);
  log.search(SearchMode::English, "spoon", 0);
  log.entry(300, Source::Search, "ir");
  log.verbTable(300, 2);
  log.phrasebook(2, PhraseAction::Opened);
  log.lessonPage(3, 1, 6, PageKind::Note);
  log.dialogueEnglish(3, 4, true);
  log.reveal(5);
  log.undo(5);
  log.setting("newPerDay", 15);
  log.clockChange(ClockKind::DayConfirmed, 100, 200);
  log.error(ErrorCode::CardFailed, 0);
  gUptime = 60000;
  CHECK(log.flush());
}

void testSampleFixture() {
  MemStore store;
  sampleSession(store);
  const std::vector<uint8_t>& bytes = store.files[kFile1];
  CHECK(!bytes.empty());
  if (FILE* out = std::fopen(kSampleOut, "wb")) {
    std::fwrite(bytes.data(), 1, bytes.size(), out);
    std::fclose(out);
  }
  // The committed fixture must be exactly what this engine writes; if the
  // encoding changes on purpose, copy build/host/usage-sample/ over it and
  // update tools/tests/test_usage_report.py.
  FILE* in = std::fopen(kFixture, "rb");
  CHECK(in != nullptr);
  if (!in) return;
  std::vector<uint8_t> fixture;
  int ch;
  while ((ch = std::fgetc(in)) != EOF) fixture.push_back(static_cast<uint8_t>(ch));
  std::fclose(in);
  CHECK(fixture == bytes);
}

}  // namespace

int main() {
  testEncoding();
  testTextCaps();
  testGuestAndSetting();
  testFullBufferAndDrops();
  testTornChunk();
  testRotation();
  testSampleFixture();
  return result();
}

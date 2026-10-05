#include "core/usage/UsageLog.h"

#include <cstdio>
#include <cstring>

#include "core/srs/Bytes.h"

namespace tinta::core::usage {
namespace {

// Kept free at the end of the buffer so a Dropped record always fits.
constexpr uint16_t kReserve = UsageLog::kRecordHeader + 4;
constexpr uint8_t kSeqBytes = 8;  // "TS", seq u16, zero u16, check u16
constexpr uint16_t kMaxSeq = 9999;

// Bytes of `text` kept under `cap`, cut before a character that would not fit
// whole.
uint8_t textLength(const char* text, uint8_t cap) {
  if (!text) return 0;
  size_t n = 0;
  while (n <= cap && text[n] != '\0') ++n;
  if (n <= cap) return static_cast<uint8_t>(n);
  n = cap;
  while (n > 0 && (static_cast<uint8_t>(text[n]) & 0xC0) == 0x80) --n;
  return static_cast<uint8_t>(n);
}

// Writes fields front to back.
struct Fields {
  uint8_t* p;
  void u8(uint8_t v) { *p++ = v; }
  void u16(uint16_t v) {
    putU16(p, v);
    p += 2;
  }
  void u32(uint32_t v) {
    putU32(p, v);
    p += 4;
  }
  void text(const char* s, uint8_t length) {
    *p++ = length;
    if (length) std::memcpy(p, s, length);
    p += length;
  }
};

void copyText(char* out, size_t cap, const char* text, uint8_t limit) {
  const uint8_t n = textLength(text, limit);
  if (n && n < cap) std::memcpy(out, text, n);
  out[n < cap ? n : 0] = '\0';
}

}  // namespace

void UsageLog::fileName(const uint16_t seq, char (&out)[24]) const {
  std::snprintf(out, sizeof out, "usage-%04u.log", static_cast<unsigned>(seq));
}

void UsageLog::open(const bool enabled) {
  opened_ = true;
  enabled_ = enabled;
  seq_ = 1;
  sizeKnown_ = false;
  if (!store_.available()) return;
  uint8_t raw[kSeqBytes];
  if (store_.read(kSeqFile, 0, raw, sizeof raw) == static_cast<int32_t>(sizeof raw) && raw[0] == 'T' && raw[1] == 'S' &&
      getU16(raw + 6) == static_cast<uint16_t>(crc32(raw, 6))) {
    const uint16_t seq = getU16(raw + 2);
    if (seq >= 1 && seq <= kMaxSeq) seq_ = seq;
  }
  char name[24];
  fileName(seq_, name);
  const int32_t size = store_.size(name);
  fileBytes_ = size > 0 ? static_cast<uint32_t>(size) : 0;
  sizeKnown_ = store_.available();
}

void UsageLog::setEnabled(const bool on) {
  if (on == enabled_) return;
  if (!on) {
    if (uint8_t* p = begin(Type::LogState, 1)) *p = 0;
    enabled_ = false;
    return;
  }
  enabled_ = true;
  if (uint8_t* p = begin(Type::LogState, 1)) *p = 1;
  if (booted_) bootRecord(BootReason::Enabled);
}

bool UsageLog::recording() const { return opened_ && enabled_ && store_.available(); }

bool UsageLog::needsFlush() const { return used_ >= (kBufferBytes - kChunkHeader) * 3 / 4; }

uint8_t* UsageLog::begin(const Type type, const uint8_t fields) {
  if (!recording()) return nullptr;
  if (unreported_ > 0 && type != Type::Dropped) {
    if (uint8_t* p = begin(Type::Dropped, 4)) {
      putU32(p, unreported_);
      unreported_ = 0;
    }
  }
  const uint16_t need = static_cast<uint16_t>(kRecordHeader + fields);
  const uint16_t room = static_cast<uint16_t>(kBufferBytes - kChunkHeader - used_);
  if (need + (type == Type::Dropped ? 0 : kReserve) > room) {
    if (type != Type::Dropped) {
      ++dropped_;
      ++unreported_;
    }
    return nullptr;
  }
  uint8_t* p = buffer_ + kChunkHeader + used_;
  p[0] = static_cast<uint8_t>(type);
  p[1] = fields;
  putU32(p + 2, uptimeMs_ ? uptimeMs_() : 0);
  used_ = static_cast<uint16_t>(used_ + need);
  ++records_;
  return p + kRecordHeader;
}

bool UsageLog::appendChunk(const char* file, uint8_t* chunk, const uint16_t bytes) {
  const uint16_t body = static_cast<uint16_t>(bytes - kChunkHeader);
  chunk[0] = 'T';
  chunk[1] = 'U';
  putU16(chunk + 2, body);
  putU16(chunk + 4, static_cast<uint16_t>(crc32(chunk + kChunkHeader, body)));
  chunk[6] = clock_.hasTimeOfDay() ? 1 : 0;
  chunk[7] = kLayout;
  putU32(chunk + 8, clock_.nowSeconds());
  putU32(chunk + 12, uptimeMs_ ? uptimeMs_() : 0);
  if (!store_.append(file, chunk, bytes)) {
    sizeKnown_ = false;
    return false;
  }
  fileBytes_ += bytes;
  return true;
}

bool UsageLog::startNextFile() {
  seq_ = static_cast<uint16_t>(seq_ >= kMaxSeq ? 1 : seq_ + 1);
  uint8_t raw[kSeqBytes] = {'T', 'S'};
  putU16(raw + 2, seq_);
  putU16(raw + 6, static_cast<uint16_t>(crc32(raw, 6)));
  store_.replace(kSeqFile, raw, sizeof raw);
  char name[24];
  // A file left from before a lost usage.seq or a wrap would mix two runs.
  fileName(seq_, name);
  if (store_.size(name) >= 0) store_.remove(name);
  const uint16_t oldest = static_cast<uint16_t>(seq_ > kKeepFiles ? seq_ - kKeepFiles : seq_ + kMaxSeq - kKeepFiles);
  fileName(oldest, name);
  if (store_.size(name) >= 0) store_.remove(name);
  fileBytes_ = 0;
  sizeKnown_ = true;
  if (!booted_) return true;

  // The new file's first chunk: this boot's record, so the file decodes alone.
  uint8_t chunk[kChunkHeader + kRecordHeader + 3 * (kVersionCap + 1) + 16] = {};
  const uint8_t device = textLength(device_, kNameCap);
  const uint8_t build = textLength(build_, kNameCap);
  const uint8_t version = textLength(version_, kVersionCap);
  const uint8_t fields = static_cast<uint8_t>(1 + 3 + device + build + version + 4 + 2 + 4 + 4 + 2);
  uint8_t* p = chunk + kChunkHeader;
  p[0] = static_cast<uint8_t>(Type::Boot);
  p[1] = fields;
  putU32(p + 2, uptimeMs_ ? uptimeMs_() : 0);
  Fields f{p + kRecordHeader};
  f.u8(static_cast<uint8_t>(BootReason::FileStart));
  f.text(device_, device);
  f.text(build_, build);
  f.text(version_, version);
  f.u32(boot_.packEdition);
  f.u8(boot_.packMajor);
  f.u8(boot_.packMinor);
  f.u32(boot_.packCrc);
  f.u32(boot_.packBuildTime);
  f.u16(clock_.today());
  fileName(seq_, name);
  return appendChunk(name, chunk, static_cast<uint16_t>(kChunkHeader + kRecordHeader + fields));
}

bool UsageLog::flush() {
  if (used_ == 0) return true;
  // A guest's records go nowhere, uncounted; a failed write's are counted.
  const bool attempt = opened_ && store_.available();
  bool ok = false;
  if (attempt) {
    char name[24];
    if (!sizeKnown_) {
      fileName(seq_, name);
      const int32_t size = store_.size(name);
      fileBytes_ = size > 0 ? static_cast<uint32_t>(size) : 0;
      sizeKnown_ = true;
    }
    const uint16_t bytes = static_cast<uint16_t>(kChunkHeader + used_);
    if (fileBytes_ > 0 && fileBytes_ + bytes > kFileCap) startNextFile();
    fileName(seq_, name);
    ok = appendChunk(name, buffer_, bytes);
  }
  if (attempt && !ok) {
    dropped_ += records_;
    unreported_ += records_;
  }
  used_ = 0;
  records_ = 0;
  return ok;
}

// ---- Device ----

void UsageLog::boot(const BootInfo& info) {
  boot_ = info;
  copyText(device_, sizeof device_, info.device, kNameCap);
  copyText(build_, sizeof build_, info.build, kNameCap);
  copyText(version_, sizeof version_, info.version, kVersionCap);
  boot_.device = device_;
  boot_.build = build_;
  boot_.version = version_;
  booted_ = true;
  bootRecord(info.reason);
}

void UsageLog::bootRecord(const BootReason reason) {
  const uint8_t device = textLength(device_, kNameCap);
  const uint8_t build = textLength(build_, kNameCap);
  const uint8_t version = textLength(version_, kVersionCap);
  uint8_t* p = begin(Type::Boot, static_cast<uint8_t>(1 + 3 + device + build + version + 4 + 2 + 4 + 4 + 2));
  if (!p) return;
  Fields f{p};
  f.u8(static_cast<uint8_t>(reason));
  f.text(device_, device);
  f.text(build_, build);
  f.text(version_, version);
  f.u32(boot_.packEdition);
  f.u8(boot_.packMajor);
  f.u8(boot_.packMinor);
  f.u32(boot_.packCrc);
  f.u32(boot_.packBuildTime);
  f.u16(clock_.today());
}

void UsageLog::sleep(const SleepCause cause, const uint8_t batteryPercent, const bool charging,
                     const uint32_t wakeAfterSeconds) {
  uint8_t* p = begin(Type::Sleep, 7);
  if (!p) return;
  Fields f{p};
  f.u8(static_cast<uint8_t>(cause));
  f.u8(batteryPercent);
  f.u8(charging ? 1 : 0);
  f.u32(wakeAfterSeconds);
}

void UsageLog::battery(const uint8_t percent, const bool chargingKnown, const bool charging) {
  uint8_t* p = begin(Type::Battery, 2);
  if (!p) return;
  p[0] = percent;
  p[1] = static_cast<uint8_t>((chargingKnown ? 1 : 0) | (charging ? 2 : 0));
}

void UsageLog::clockChange(const ClockKind kind, const uint32_t before, const uint32_t after) {
  uint8_t* p = begin(Type::ClockChange, 11);
  if (!p) return;
  Fields f{p};
  f.u8(static_cast<uint8_t>(kind));
  f.u32(before);
  f.u32(after);
  f.u16(clock_.today());
}

void UsageLog::error(const ErrorCode code, const uint32_t detail) {
  uint8_t* p = begin(Type::Error, 5);
  if (!p) return;
  Fields f{p};
  f.u8(static_cast<uint8_t>(code));
  f.u32(detail);
}

void UsageLog::setting(const char* name, const int32_t value) {
  const uint8_t n = textLength(name, kNameCap);
  uint8_t* p = begin(Type::Setting, static_cast<uint8_t>(4 + 1 + n));
  if (!p) return;
  Fields f{p};
  f.u32(static_cast<uint32_t>(value));
  f.text(name, n);
}

// ---- Interface ----

void UsageLog::screen(const uint8_t screen, const uint8_t depth, const uint8_t how) {
  uint8_t* p = begin(Type::Screen, 3);
  if (!p) return;
  p[0] = screen;
  p[1] = depth;
  p[2] = how;
}

void UsageLog::input(const Input input, const Outcome outcome, const uint8_t screen, const uint16_t x,
                     const uint16_t y) {
  uint8_t* p = begin(Type::Input, 7);
  if (!p) return;
  Fields f{p};
  f.u8(static_cast<uint8_t>(input));
  f.u8(static_cast<uint8_t>(outcome));
  f.u8(screen);
  f.u16(x);
  f.u16(y);
}

void UsageLog::frame(const Refresh refresh, const uint16_t latencyMs, const uint16_t presentMs, const uint8_t screen) {
  uint8_t* p = begin(Type::Frame, 6);
  if (!p) return;
  Fields f{p};
  f.u8(static_cast<uint8_t>(refresh));
  f.u16(latencyMs);
  f.u16(presentMs);
  f.u8(screen);
}

// ---- Learning ----

void UsageLog::sessionStart(const SessionKind kind, const uint16_t tag, const uint16_t planned, const uint16_t due,
                            const uint16_t fresh, const bool resumed) {
  uint8_t* p = begin(Type::SessionStart, 10);
  if (!p) return;
  Fields f{p};
  f.u8(static_cast<uint8_t>(kind));
  f.u16(tag);
  f.u16(planned);
  f.u16(due);
  f.u16(fresh);
  f.u8(resumed ? 1 : 0);
}

void UsageLog::sessionEnd(const SessionEndHow how, const uint16_t done, const uint16_t correct,
                          const uint32_t seconds) {
  uint8_t* p = begin(Type::SessionEnd, 9);
  if (!p) return;
  Fields f{p};
  f.u8(static_cast<uint8_t>(how));
  f.u16(done);
  f.u16(correct);
  f.u32(seconds);
}

void UsageLog::itemShown(const uint32_t uid, const uint8_t format, const uint8_t kind, const uint16_t lesson,
                         const uint8_t reps, const char* const* options, uint8_t optionCount, const uint8_t answer) {
  if (!options) optionCount = 0;
  if (optionCount > kMaxOptions) optionCount = kMaxOptions;
  uint8_t lengths[kMaxOptions] = {};
  uint16_t fields = 4 + 1 + 1 + 2 + 1 + 1 + 1;
  for (uint8_t i = 0; i < optionCount; ++i) {
    lengths[i] = textLength(options[i], kOptionCap);
    fields = static_cast<uint16_t>(fields + 1 + lengths[i]);
  }
  uint8_t* p = begin(Type::ItemShown, static_cast<uint8_t>(fields));
  if (!p) return;
  Fields f{p};
  f.u32(uid);
  f.u8(format);
  f.u8(kind);
  f.u16(lesson);
  f.u8(reps);
  f.u8(answer);
  f.u8(optionCount);
  for (uint8_t i = 0; i < optionCount; ++i) f.text(options[i], lengths[i]);
}

void UsageLog::answer(const uint32_t uid, const uint8_t format, const uint8_t chosen, const Correct correct,
                      const uint8_t grade, const uint32_t responseMs, const uint8_t attempts, const char* typed) {
  const uint8_t n = textLength(typed, kTypedCap);
  uint8_t* p = begin(Type::Answer, static_cast<uint8_t>(4 + 1 + 1 + 1 + 1 + 4 + 1 + 1 + n));
  if (!p) return;
  Fields f{p};
  f.u32(uid);
  f.u8(format);
  f.u8(chosen);
  f.u8(static_cast<uint8_t>(correct));
  f.u8(grade);
  f.u32(responseMs);
  f.u8(attempts);
  f.text(typed, n);
}

void UsageLog::reveal(const uint32_t uid) {
  if (uint8_t* p = begin(Type::Reveal, 4)) putU32(p, uid);
}

void UsageLog::undo(const uint32_t uid) {
  if (uint8_t* p = begin(Type::Undo, 4)) putU32(p, uid);
}

void UsageLog::lessonPage(const uint16_t lesson, const uint16_t page, const uint16_t pageCount, const PageKind kind) {
  uint8_t* p = begin(Type::LessonPage, 7);
  if (!p) return;
  Fields f{p};
  f.u16(lesson);
  f.u16(page);
  f.u16(pageCount);
  f.u8(static_cast<uint8_t>(kind));
}

void UsageLog::dialogueEnglish(const uint16_t lesson, const uint16_t line, const bool shown) {
  uint8_t* p = begin(Type::DialogueEnglish, 5);
  if (!p) return;
  Fields f{p};
  f.u16(lesson);
  f.u16(line);
  f.u8(shown ? 1 : 0);
}

// ---- Reading and lookup ----

void UsageLog::storyOpen(const uint32_t story, const uint16_t pageCount) {
  uint8_t* p = begin(Type::StoryOpen, 6);
  if (!p) return;
  Fields f{p};
  f.u32(story);
  f.u16(pageCount);
}

void UsageLog::storyPage(const uint32_t story, const uint16_t page, const uint16_t pageCount) {
  uint8_t* p = begin(Type::StoryPage, 8);
  if (!p) return;
  Fields f{p};
  f.u32(story);
  f.u16(page);
  f.u16(pageCount);
}

void UsageLog::storyDone(const uint32_t story, const uint8_t right, const uint8_t total) {
  uint8_t* p = begin(Type::StoryDone, 6);
  if (!p) return;
  Fields f{p};
  f.u32(story);
  f.u8(right);
  f.u8(total);
}

void UsageLog::gloss(const uint16_t lemma, const Source source, const char* word) {
  const uint8_t n = textLength(word, kWordCap);
  uint8_t* p = begin(Type::Gloss, static_cast<uint8_t>(2 + 1 + 1 + n));
  if (!p) return;
  Fields f{p};
  f.u16(lemma);
  f.u8(static_cast<uint8_t>(source));
  f.text(word, n);
}

void UsageLog::sentenceEnglish(const uint32_t where, const uint16_t sentence, const Source source) {
  uint8_t* p = begin(Type::SentenceEnglish, 7);
  if (!p) return;
  Fields f{p};
  f.u32(where);
  f.u16(sentence);
  f.u8(static_cast<uint8_t>(source));
}

void UsageLog::star(const uint32_t uid, const bool on, const char* word) {
  const uint8_t n = textLength(word, kWordCap);
  uint8_t* p = begin(Type::Star, static_cast<uint8_t>(4 + 1 + 1 + n));
  if (!p) return;
  Fields f{p};
  f.u32(uid);
  f.u8(on ? 1 : 0);
  f.text(word, n);
}

void UsageLog::quizAnswer(const uint32_t story, const uint8_t question, const uint8_t chosen, const bool right) {
  uint8_t* p = begin(Type::QuizAnswer, 7);
  if (!p) return;
  Fields f{p};
  f.u32(story);
  f.u8(question);
  f.u8(chosen);
  f.u8(right ? 1 : 0);
}

void UsageLog::search(const SearchMode mode, const char* text, const uint16_t results) {
  const uint8_t n = textLength(text, kSearchCap);
  uint8_t* p = begin(Type::Search, static_cast<uint8_t>(1 + 2 + 1 + n));
  if (!p) return;
  Fields f{p};
  f.u8(static_cast<uint8_t>(mode));
  f.u16(results);
  f.text(text, n);
}

void UsageLog::entry(const uint16_t lemma, const Source via, const char* headword) {
  const uint8_t n = textLength(headword, kWordCap);
  uint8_t* p = begin(Type::Entry, static_cast<uint8_t>(2 + 1 + 1 + n));
  if (!p) return;
  Fields f{p};
  f.u16(lemma);
  f.u8(static_cast<uint8_t>(via));
  f.text(headword, n);
}

void UsageLog::verbTable(const uint16_t lemma, const uint8_t tense) {
  uint8_t* p = begin(Type::VerbTable, 3);
  if (!p) return;
  Fields f{p};
  f.u16(lemma);
  f.u8(tense);
}

void UsageLog::phrasebook(const uint16_t category, const PhraseAction action) {
  uint8_t* p = begin(Type::Phrasebook, 3);
  if (!p) return;
  Fields f{p};
  f.u16(category);
  f.u8(static_cast<uint8_t>(action));
}

}  // namespace tinta::core::usage

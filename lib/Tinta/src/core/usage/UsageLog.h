#pragma once

#include <cstdint>

#include "core/Clock.h"
#include "core/StateStore.h"

namespace tinta::core::usage {

// The usage log (PLAN.md 8.6): what the learner did and how the interface
// behaved, for the owner to study on a PC (tools/usage-report.py). It is not
// learner state: nothing reads it back on the device, losing it loses no
// progress, and a failed write is counted and otherwise ignored.
//
// Recording is cheap enough for input handling: each call encodes a few bytes
// into a fixed RAM buffer and touches nothing else. Only flush() writes, one
// StateStore::append per call, and only when the app asks (after its journal
// write, at sleep, and when needsFlush()). Nothing here allocates.
//
// Files: /tinta/usage-0001.log, usage-0002.log, ... A file is closed at
// kFileCap and the next begun (StateStore cannot rename, so files are
// numbered rather than rotated by name); the oldest beyond kKeepFiles is
// removed. usage.seq holds the current number. Each file starts with a Boot
// record (reason FileStart), so it decodes on its own.
//
// A flush appends one chunk:
//
//   0  'T' 'U'     magic
//   2  u16 bytes   length of the records that follow
//   4  u16 check   low half of the CRC-32 of those records
//   6  u8  flags   bit 0: wall is a time of day (Clock::hasTimeOfDay)
//   7  u8  layout  kLayout
//   8  u32 wall    Clock::nowSeconds() at the flush
//  12  u32 uptime  ms since boot at the flush
//  16  records
//
// A record:
//
//   0  u8  type    Type below; a decoder skips types it does not know
//   1  u8  size    bytes of fields after the uptime
//   2  u32 uptime  ms since boot when it was recorded
//   6  fields      per type (docs/usage-log.md); newer firmware may append
//                  fields, so decoders read what they know and skip the rest
//
// A record's wall time is the chunk's wall plus its uptime minus the chunk's.
// A cut during a flush leaves a torn chunk at the end of the file; the next
// boot appends after it, and the decoder finds the next magic and checks it.
//
// Integers are little-endian. Strings are a u8 length and UTF-8 bytes,
// capped per field (kTextCap...) and cut on a character boundary.
//
// Guest mode (no card) and the "Record usage" setting off record nothing.

enum class Type : uint8_t {
  // Engine and device
  Boot = 1,
  Dropped = 2,
  LogState = 3,
  // 4 Sleep and 5 Battery: written by the standalone firmware only.
  ClockChange = 6,
  Error = 7,
  Setting = 8,
  // Interface
  Screen = 16,
  Input = 17,
  // 18 Frame: written by the standalone firmware only.
  // Learning
  SessionStart = 32,
  SessionEnd = 33,
  ItemShown = 34,
  Answer = 35,
  Reveal = 36,
  Undo = 37,
  LessonPage = 38,
  DialogueEnglish = 39,
  // Reading and lookup
  StoryOpen = 48,
  StoryPage = 49,
  StoryDone = 50,
  Gloss = 51,
  SentenceEnglish = 52,
  Star = 53,
  QuizAnswer = 54,
  Search = 55,
  Entry = 56,
  VerbTable = 57,
  Phrasebook = 58,
};

enum class BootReason : uint8_t {
  PowerOn = 0,    // a cold start or a reset; in lila, each opening of Tinta
  WakeKey = 1,    // standalone: deep sleep, Power key
  WakeTimer = 2,  // standalone: deep sleep, timer (the rotating sleep word)
  Restart = 3,    // standalone: the firmware restarted itself (an update, USB transfer)
  FileStart = 4,  // not a boot: the head of a new file, repeating this boot's
  Enabled = 5,    // not a boot: recording turned back on, repeating this boot's
};

// Which input; the app maps its events onto these.
enum class Input : uint8_t {
  Back = 0,
  Confirm = 1,
  Left = 2,
  Right = 3,
  Up = 4,
  Down = 5,
  Power = 6,
  Home = 7,      // X4 Pro Home pad, tapped
  BackHold = 8,  // Back held (the pause sheet)
  HomeHold = 9,  // Home pad held
  Tap = 10,      // with x, y
  Swipe = 11,    // with the start x, y
};

enum class Outcome : uint8_t {
  Handled = 0,  // it did something
  Ignored = 1,  // it did nothing on that screen: a dead press
  Queued = 2,   // it arrived during a refresh and waited for the next frame
};

enum class ClockKind : uint8_t {
  // 0 Set and 2 TimeZone: written by the standalone firmware only.
  DayConfirmed = 1,  // the date confirmed without a trusted clock
  DayRollover = 3,   // the study day changed while awake
};

enum class ErrorCode : uint8_t {
  CardFailed = 0,     // the card stopped answering
  PackError = 1,      // the course pack did not open; detail = the pack's status
  ProgressGuest = 2,  // the progress store fell back to guest; detail = its result
  SessionLost = 3,    // a saved session could not be restored
  Other = 255,
};

enum class SessionKind : uint8_t {
  Today = 0,
  Lesson = 1,   // a lesson's practice; tag = lesson
  Phrases = 2,  // a phrasebook category; tag = category
  Starred = 3,  // words starred while reading
  Other = 255,
};

enum class SessionEndHow : uint8_t {
  Finished = 0,  // the queue ran out
  Ended = 1,     // End session in the pause sheet
  Slept = 2,     // set aside for sleep (it may resume)
  Left = 3,      // left without ending (Home, Back out)
  Dropped = 4,   // a saved session from another day was dropped at boot
};

enum class Correct : uint8_t {
  Wrong = 0,
  Right = 1,
  Near = 2,        // right with a slip, a mistake or missing accents (graded Hard)
  SelfGraded = 3,  // a flashcard: the grade is the learner's
};

enum class PageKind : uint8_t { Cover = 0, Note = 1, Dialogue = 2, Word = 3, Practice = 4, Other = 255 };

enum class Source : uint8_t { Reader = 0, Dictionary = 1, Lesson = 2, Session = 3, Search = 4, Other = 255 };

enum class SearchMode : uint8_t { Spanish = 0, English = 1, Letter = 2 };

enum class PhraseAction : uint8_t { Opened = 0, Practised = 1 };

// What the boot record names, so a log joins to the right content/ids.lock.
struct BootInfo {
  BootReason reason = BootReason::PowerOn;
  const char* device = "";   // "X4CLASSIC" (platform::Board::id())
  const char* build = "";    // "x4-classic" (platform::buildName())
  const char* version = "";  // "0.7.0" (platform::version())
  uint32_t packEdition = 0;  // Pack::contentVersion()
  uint8_t packMajor = 0;
  uint8_t packMinor = 0;
  uint32_t packCrc = 0;  // the pack header's CRC field
  uint32_t packBuildTime = 0;
};

class UsageLog {
 public:
  // Fits a typical stretch between two flushes (an answer's worth of screens,
  // presses and frames) many times over; when it is three quarters full,
  // needsFlush() asks for one.
  static constexpr uint16_t kBufferBytes = 1024;
  static constexpr uint32_t kFileCap = 1024u * 1024u;
  static constexpr uint16_t kKeepFiles = 16;
  static constexpr uint8_t kLayout = 1;
  static constexpr uint8_t kChunkHeader = 16;
  static constexpr uint8_t kRecordHeader = 6;
  // Field caps for text, in bytes.
  static constexpr uint8_t kNameCap = 23;  // device, build, setting names
  static constexpr uint8_t kVersionCap = 31;
  static constexpr uint8_t kOptionCap = 24;  // each option shown
  static constexpr uint8_t kTypedCap = 32;   // a typed answer
  static constexpr uint8_t kWordCap = 24;    // a glossed word, a headword
  static constexpr uint8_t kSearchCap = 24;  // search text
  static constexpr uint8_t kMaxOptions = 4;
  static constexpr const char* kSeqFile = "usage.seq";

  using UptimeFn = uint32_t (*)();

  UsageLog(StateStore& store, const Clock& clock, UptimeFn uptimeMs)
      : store_(store), clock_(clock), uptimeMs_(uptimeMs) {}

  // Once the store is up: reads usage.seq and the current file's size (two
  // small reads). `enabled` is the profile's "Record usage".
  void open(bool enabled);
  bool enabled() const { return enabled_; }
  // Turning it off records that it was turned off (flushed with the next
  // flush()); turning it on repeats the boot record, so what follows has its
  // context.
  void setEnabled(bool on);

  // True when the buffer is three quarters full: flush at the next quiet
  // moment.
  bool needsFlush() const;
  // Appends what is buffered as one chunk. True when it was written or there
  // was nothing to write; false (records counted as dropped) when the write
  // failed or there is no card.
  bool flush();

  // Records lost since boot: the buffer was full, a record would not fit, a
  // write failed.
  uint32_t dropped() const { return dropped_; }
  uint16_t buffered() const { return used_; }
  uint16_t fileNumber() const { return seq_; }

  // ---- Device ----
  // First, at every boot, once the pack is open. Kept to head each new file.
  void boot(const BootInfo& info);
  void clockChange(ClockKind kind, uint32_t before, uint32_t after);
  void error(ErrorCode code, uint32_t detail);
  // A setting changed: its profile name ("newPerDay") and new value.
  void setting(const char* name, int32_t value);

  // ---- Interface ----
  // A screen became the top of the stack. `screen` is the app's ScreenId.
  // how: 0 push, 1 pop, 2 reset, 3 restored at boot.
  void screen(uint8_t screen, uint8_t depth, uint8_t how);
  // x, y: logical coordinates of a tap or a swipe's start, else 0xFFFF.
  void input(Input input, Outcome outcome, uint8_t screen, uint16_t x = 0xFFFF, uint16_t y = 0xFFFF);

  // ---- Learning ----
  // resumed: the session came back after a wake rather than starting.
  void sessionStart(SessionKind kind, uint16_t tag, uint16_t planned, uint16_t due, uint16_t fresh,
                    bool resumed = false);
  void sessionEnd(SessionEndHow how, uint16_t done, uint16_t correct, uint32_t seconds);
  // A card as asked: `format` is the journal's exercise format code, `kind`
  // the ItemKind, `lesson` the item's lesson (0xFFFF none), `reps` its grades
  // so far. options: the texts shown, in order (none for flashcards and
  // tiles); answer: the right one's index (0xFF none).
  void itemShown(uint32_t uid, uint8_t format, uint8_t kind, uint16_t lesson, uint8_t reps, const char* const* options,
                 uint8_t optionCount, uint8_t answer);
  // chosen: the option index (0xFF none); typed: what was typed (nullptr
  // none); attempts: wrong tiles or tries before it was right.
  void answer(uint32_t uid, uint8_t format, uint8_t chosen, Correct correct, uint8_t grade, uint32_t responseMs,
              uint8_t attempts, const char* typed = nullptr);
  void reveal(uint32_t uid);
  void undo(uint32_t uid);
  void lessonPage(uint16_t lesson, uint16_t page, uint16_t pageCount, PageKind kind);
  void dialogueEnglish(uint16_t lesson, uint16_t line, bool shown);

  // ---- Reading and lookup ----
  // story: the reading's stable key (library::storyKey()).
  void storyOpen(uint32_t story, uint16_t pageCount);
  void storyPage(uint32_t story, uint16_t page, uint16_t pageCount);
  void storyDone(uint32_t story, uint8_t right, uint8_t total);
  // lemma: the pack's lemma id (this edition only), word: the headword.
  void gloss(uint16_t lemma, Source source, const char* word);
  // `where`: the story key, or the lesson; sentence: the pack's sentence id.
  void sentenceEnglish(uint32_t where, uint16_t sentence, Source source);
  void star(uint32_t uid, bool on, const char* word);
  void quizAnswer(uint32_t story, uint8_t question, uint8_t chosen, bool right);
  void search(SearchMode mode, const char* text, uint16_t results);
  void entry(uint16_t lemma, Source via, const char* headword);
  void verbTable(uint16_t lemma, uint8_t tense);
  void phrasebook(uint16_t category, PhraseAction action);

 private:
  // Starts a record of `fields` bytes in the buffer, or returns nullptr (the
  // record is dropped) when off, a guest, or out of room.
  uint8_t* begin(Type type, uint8_t fields);
  bool recording() const;
  void bootRecord(BootReason reason);
  bool appendChunk(const char* file, uint8_t* chunk, uint16_t bytes);
  bool startNextFile();
  void fileName(uint16_t seq, char (&out)[24]) const;

  StateStore& store_;
  const Clock& clock_;
  UptimeFn uptimeMs_;

  // The chunk header's room, then records.
  uint8_t buffer_[kBufferBytes] = {};
  uint16_t used_ = 0;  // record bytes after the header room
  uint16_t records_ = 0;

  BootInfo boot_{};
  // The boot's strings, copied: the caller's may not outlive boot().
  char device_[kNameCap + 1] = {};
  char build_[kNameCap + 1] = {};
  char version_[kVersionCap + 1] = {};
  bool booted_ = false;

  bool opened_ = false;
  bool enabled_ = false;
  uint16_t seq_ = 1;
  uint32_t fileBytes_ = 0;
  bool sizeKnown_ = false;
  uint32_t dropped_ = 0;
  uint32_t unreported_ = 0;  // dropped records not yet in a Dropped record
};

}  // namespace tinta::core::usage

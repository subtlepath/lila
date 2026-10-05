# The usage log

What the learner did and how the interface behaved, recorded on the SD card
for the owner to study on a PC (PLAN.md 8.6). The engine is
`src/core/usage/UsageLog` (host-tested in `test/tinta/usage_log_test.cpp`);
`tools/usage-report.py` decodes it and writes the report. The header comment
in `UsageLog.h` is the contract; this page explains it.

## Rules

- **Not learner state.** Nothing on the device reads it back. Losing it loses
  no progress. The review journal is always written first, and a failed usage
  write is counted and otherwise ignored.
- **Always on, except as a guest.** There is no setting to turn it off yet
  (`UsageLog::setEnabled()` is ready for one). Without a card nothing is
  recorded or counted.
- **Cheap to record.** Each call encodes a few bytes into a 1 KB RAM buffer and
  touches nothing else, so it can be called from input handling. Nothing
  allocates.
- **Written only by `flush()`, when the app asks.** The app calls it right
  after its journal write, when Tinta closes (lila sleeps or goes elsewhere),
  and when `needsFlush()` says the buffer
  is three quarters full. Each flush is one `StateStore::append` of one chunk.
  On the device that is one more file open per answered item: Storage keeps
  one file open, and the usage file and `reviews.log` take turns.
- **Dropped records** (buffer full, a failed write) are counted. A `dropped`
  record says how many were lost at the point where they went missing.

## Files

`/tinta/usage-0001.log`, `usage-0002.log`, … A file is closed at 1 MiB and
the next one begun. The oldest beyond 16 files is removed. `usage.seq`
(8 bytes: `TS`, the number u16, zero u16, CRC-16) holds the current number;
it is rewritten atomically only when a file begins. Numbers run to 9999 and
then wrap to 1; the report puts them back in order.

PLAN.md named a single `usage.log`. `StateStore` has no rename, so instead of
rotating one file by name the files are numbered.

In lila each opening of Learn Spanish counts as a boot. Each file begins with
a `boot` record (reason `file-start`, repeating this boot's), so any file
decodes on its own and joins to the right `ids.lock`.

## Layout

All integers are little-endian. Strings are a u8 length and UTF-8 bytes,
capped per field and cut on a character boundary.

**Chunk**, one per flush:

| Offset | Field | |
|---|---|---|
| 0 | `T` `U` | magic |
| 2 | u16 bytes | length of the records that follow |
| 4 | u16 check | low 16 bits of the CRC-32 of those records |
| 6 | u8 flags | bit 0: `wall` is a time of day (`Clock::hasTimeOfDay`) |
| 7 | u8 layout | 1 |
| 8 | u32 wall | `Clock::nowSeconds()` at the flush: seconds since 2024-01-01, local |
| 12 | u32 uptime | ms since boot at the flush |
| 16 | records | |

**Record:**

| Offset | Field | |
|---|---|---|
| 0 | u8 type | below; decoders skip types they do not know |
| 1 | u8 size | bytes of fields after the uptime |
| 2 | u32 uptime | ms since boot when it was recorded |
| 6 | fields | |

A record's wall time is the chunk's `wall` minus (the chunk's `uptime` minus
the record's) / 1000. Without a time of day (the X4, or an RTC Tinta does not
trust), `wall` is the confirmed day plus uptime, so it gives the date but not
the hour.

A newer firmware may append fields to a record; decoders read the fields they
know and skip to `size`. A record shorter than a decoder expects came from an
older firmware; the missing fields are absent.

A power cut during a flush leaves a torn chunk at the end of the file. The
next boot appends after it. The decoder skips bytes until the next `TU` whose
check matches.

## Records

`str` is a capped string. Enumerations are u8; the names are those in
`UsageLog.h` and in the report. Rows marked *standalone* were written only by
the standalone Tinta firmware; lila does not record them, and the report
still decodes them from older logs.

### Device and engine

| Type | Name | Fields |
|---|---|---|
| 1 | boot | reason (power-on, wake-key, wake-timer, restart, file-start, enabled), device str (`X4CLASSIC`), build str (`x4-classic`), version str (`0.7.0`), pack edition u32, pack format major u8, minor u8, pack CRC u32, pack build time u32, study day u16 |
| 2 | dropped | count u32: records lost just before this point |
| 3 | log_state | on u8 (the setting changed) |
| 4 | sleep | *standalone*: cause (key, idle, menu, low-battery, rotation), battery % u8 (255 unknown), charging u8, timer wake after s u32 (0 none) |
| 5 | battery | *standalone*: % u8 (255 unknown), flags u8 (bit 0 charging known, bit 1 charging) |
| 6 | clock_change | kind (day-confirmed, day-rollover; set and time-zone *standalone*), before u32, after u32 (nowSeconds), study day u16 |
| 7 | error | code (card-failed, pack-error, progress-guest, session-lost, other), detail u32 |
| 8 | setting | value i32, name str (the profile field, `newPerDay`) |

### Interface

| Type | Name | Fields |
|---|---|---|
| 16 | screen | screen u8 (`app::ScreenId`), stack depth u8, how u8 (push, pop, reset, restored) |
| 17 | input | input (back, confirm, left, right, up, down, power, home, back-hold, home-hold, tap, swipe), outcome (handled, ignored = nothing on screen changed, queued = handled while the last frame was still going to the panel), screen u8, x u16, y u16 (logical; 65535 none) |
| 18 | frame | *standalone*: refresh (full, half, fast, window), press-to-frame ms u16 (65535: no press), present ms u16, screen u8 |

### Learning

| Type | Name | Fields |
|---|---|---|
| 32 | session_start | kind (today, lesson, phrases, starred), tag u16 (lesson or category), planned u16, due u16, new u16, resumed u8 (1: the session came back after a wake; a session set aside for sleep ends `slept` with its totals so far) |
| 33 | session_end | how (finished, ended, slept, left, dropped), done u16, right u16, seconds u32 |
| 34 | item_shown | uid u32, format u8 (`ui::ExerciseFormat`, the journal's code), kind u8 (`ItemKind`), lesson u16 (65535 none), reps u8, answer index u8 (255 none), options: count u8 then each option's text str (24 bytes) |
| 35 | answer | uid u32, format u8, chosen option u8 (255 none), correct (wrong, right, near = right with a slip or missing accents, self-graded), grade u8 (1–4), response ms u32, attempts u8 (wrong tiles or tries), typed str (32 bytes; empty when nothing was typed) |
| 36 | reveal | uid u32 (a flashcard turned) |
| 37 | undo | uid u32 |
| 38 | lesson_page | lesson u16, page u16, page count u16, kind (cover, note, dialogue, word, practice) |
| 39 | dialogue_english | lesson u16, line u16, shown u8 |

### Reading and lookup

| Type | Name | Fields |
|---|---|---|
| 48 | story_open | story key u32 (`library::storyKey`), page count u16 |
| 49 | story_page | story key u32, page u16, page count u16 |
| 50 | story_done | story key u32, quiz right u8, quiz total u8 |
| 51 | gloss | lemma u16 (this pack edition), source (reader, dictionary, lesson, session, search), word str (24 bytes) |
| 52 | sentence_english | story key or lesson u32, sentence id u16, source |
| 53 | star | uid u32, on u8, word str |
| 54 | quiz_answer | story key u32, question u8, chosen u8, right u8 |
| 55 | search | mode (spanish, english, letter), results u16, text str (24 bytes) |
| 56 | entry | lemma u16, via (source), headword str |
| 57 | verb_table | lemma u16, tense u8 |
| 58 | phrasebook | category u16, action (opened, practised) |

Types 9–15, 19–31, 40–47 and 59 on are free. A new type goes in the header,
here, and in `TYPES` in `tools/usage-report.py`.

## Size

A 20-minute day — one session of 40 items, about 120 presses, a lesson, a few
searches and a reading — comes to about **7.5 KB**: 103 bytes per answered
item (the card with its options, the answer, two presses), 2.7 KB of
navigation, and 16 bytes per flush. A 1 MiB file lasts about four and a half
months; the 16 files kept hold about six years.

RAM: 1 KB of buffer and about 100 bytes of state on the C3.

## Where the app records

The engine records nothing on its own. These are the call sites, all on the
loop task. `log` stands for the App's `UsageLog`.

| Where | Call |
|---|---|
| `App::open()`, once the store is open | `log.open(true)`, then once the pack is open `log.boot({power-on, board.id(), "lila", lila's version, pack edition, major, minor, CRC, build time})` |
| `App` after every journal write (a grade, an undo, a suspend) | `log.flush()` |
| `App::update()` | `if (log.needsFlush()) log.flush()` at a quiet moment (not mid-present) |
| `App::close()` (lila sleeps or leaves Tinta) | `log.flush()` |
| The date prompt and picker; `pollDayChange()` | `log.clockChange(day-confirmed or day-rollover, ...)` |
| Card failure, pack error, guest progress store, dropped saved session | `log.error(...)` |
| Settings: each field change | `log.setting(field key, value)` (the `Field` name in `SettingsScreens.cpp`) |
| `App` push / pop / resetTo, and the resumed stack at boot | `log.screen(id, depth, how)` |
| `App::handle()`: every input event, after routing | `log.input(input, outcome, topId())`: ignored when nothing on screen changed, queued when it was handled while the last frame was still going to the panel |
| `SessionController` build and end | `log.sessionStart(kind, tag, planned, due, new)`, `log.sessionEnd(how, done, right, seconds)` |
| `SessionController::showCurrent()`, each card shown (not from a build) | `log.itemShown(uid, format, kind, lesson, reps, option texts, count, answer)` |
| `SessionController` on a grade | `log.answer(...)`, before the journal write's `flush()` |
| Flashcard reveal; undo | `log.reveal(uid)`, `log.undo(uid)` |
| `LessonScreen` page change; dialogue English toggle | `log.lessonPage(...)`, `log.dialogueEnglish(...)` |
| `ReaderScreen` open, page turn, gloss, star, sentence English; `QuizScreen` | `log.storyOpen/storyPage/gloss/star/sentenceEnglish/quizAnswer/storyDone` |
| `SearchScreen` when a result is opened or the screen left (the query as it stood, not each letter); the letter sheet at each prefix; dictionary entry and verb table | `log.search(mode, text, results)`, `log.entry(...)`, `log.verbTable(...)` |
| Phrasebook category opened, practice started | `log.phrasebook(category, action)` |

## The report

```sh
python3 tools/usage-report.py /Volumes/CARD --out ~/tinta-usage     # the card's root, or its tinta/
python3 tools/usage-report.py SD --ids content/ids.lock --dump build/course.dump.txt
```

It reads every `usage-NNNN.log` in order and writes `report.md`,
`events.jsonl` (every record) and `csv/<type>.csv`. Items are named from
`content/ids.lock` and, if given, a pack listing (`python3 tools/packc --dump`,
written next to the pack); use the `ids.lock` of the edition the boot record
names. Screens are named from `src/app/View.h`.

The report covers:
- accuracy and median response time by format, by lesson and by word (the
  hardest first);
- the wrong options chosen most and typed answers that were not quite right;
- sessions by kind and how they ended, and how far lessons were read;
- words glossed most, searches (and those that found nothing: candidate
  headwords), entries opened, starred words, readings opened and finished;
- time per screen, screen-to-screen paths, and presses that did nothing or
  waited for a refresh;
- boots (openings), errors and settings;
- from the standalone firmware's logs only: press-to-frame latency by refresh
  kind, sleeps, and battery use per hour awake and asleep.

Tests: `python3 -m unittest discover -s tools/tests -p 'test_usage*'` decodes
a sample written by the firmware's own encoder
(`test/tinta/fixtures/usage-sample/`), which `test/tinta/usage_log_test.cpp`
checks byte for byte, so the two cannot drift apart unnoticed.

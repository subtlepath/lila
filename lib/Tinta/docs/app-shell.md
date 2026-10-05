# App shell (M2 to M5 UI)

How the firmware around the course is put together, and how to extend it.
PLAN.md sections 4.4–4.7, 6.4–6.8 and 8 are the design; this is the code.

## Pieces

| Where | What |
|---|---|
| `src/main.cpp` | Builds a new `app::App` in `setup()` (also after a simulated wake) |
| `src/app/App` | Boot, the course and progress, the screen stack, input dispatch, refresh policy, profile and session saving, sleep, low battery |
| `src/app/SessionController` | The review session (Today, or a lesson's practice): queue, format per card, card and side, grades, undo, leeches, day totals, lesson completion, its part of `session.bin` |
| `src/app/View.h` | The screen interface, `ScreenId`, shared action ids |
| `src/app/Screens.cpp` | One instance of each screen, by `ScreenId` |
| `src/assets/CoursePack.cpp` | The course pack, `.incbin`'d into the image (device builds) |
| `src/ui/KeyMap` | Raw input to keys/taps/swipes; front-key order and footer geometry; choice bars |
| `src/ui/Theme` | Per-device FreeInkUI tokens, focus styles, font slots |
| `src/ui/Strings` | Every chrome string, English and Spanish |
| `src/ui/views/Chrome` | Status bar, footer over the front keys (choice bar or key hints), banners |
| `src/ui/views/FormView` | Rows: menus, settings pages, pickers, the pause sheet, the summary |
| `src/ui/views/ExerciseView.h` | The interface an exercise format implements; the format codes |
| `src/ui/views/Cards` | Self-graded formats: `FlashcardView`, `RevealCardView` |
| `src/ui/views/Exercises` | Auto-graded formats: `ChoiceView` (meaning, word, gap, article, form), `WordOrderView`, `TypedView` (X4 Pro keyboard); the registry |
| `src/ui/views/SpanishKeyboard` | The X4 Pro keyboard with the accent row (typed answers, search) |
| `src/ui/views/ListView` | Lists longer than a form: the readings, the phrasebook |
| `src/ui/TextBuffers` | One span and run buffer for the screens that typeset pages (notes, the reader) |
| `src/core/library/` | M6 lookups: readings, story keys, a lemma's recognise item, a category's phrases, a word's lemma; `MarkLog` (starred words, readings read); the sleep screen's word |
| `src/core/reader/` | A story as typesetter spans, a paragraph at a time, every word numbered |
| `src/core/search/` | Dictionary search (headword, inflected form, prefix, English) and the next letters of a prefix |
| `src/core/session/` | Format picking, options, word-order tiles, grades from answers, a lesson's practice list (pure, host-tested) |
| `src/core/lang/AnswerCheck` | Typed answers: case, accents, one slip, non-Mexican synonyms |
| `src/ui/views/CardText` | Typeset blocks for cards and the dictionary: label line, headword, sentence with the word in bold, labelled values |
| `src/ui/screens/*` | Home, Session and Summary, Lesson and Course map, Readings, Reader and Questions, Phrasebook, Progress, Dictionary (list, letters, entry, entry actions, verb table, search), Settings, Update from SD card and USB transfer (`DeviceScreens`), the first run (`WelcomeScreens`), About and Licences, Diagnostics, pack error, Specimen, Input test, sheets, date and clock, sleep and low-battery screens |
| `src/platform/*` | Board (input task, present), Clock, Storage, Card, Power, Light, Log, PackImage |

## Boot

`App::setup()`: power, board, card, profile; then the course
(`openCourse()`): the pack's bytes from `platform::packImage()` go to
`Pack::open()` (structural checks only; `verifyCrc()` runs from Diagnostics >
Check course data), the FSRS model is configured from the profile, the slot
table is allocated (2 bytes per pack item: the only allocation that grows
with the course) and `ProgressStore::open()` replays or rebuilds what it must
(logged as `[tinta] progress <result> in <ms>`). Then the clock: an RTC
reading before the later of the profile's last day and the journal's last
review day is not trusted, and the date is asked first. A battery under 5 %
goes straight to the low-battery screen and sleeps. Without a pack the root
screen is the pack error screen instead of Home (`App::rootId()`).

## The course pack

- **Device**: `extra_scripts = pre:tools/build_assets.py` compiles a scratch
  copy of `content/` into `build/assets/course.pack` (plain packc, no
  relaxations; `content/ids.lock` is never written) and a header naming it
  and carrying its CRC.
  `src/assets/CoursePack.cpp` `.incbin`s it into `.rodata`, which the flash
  MMU maps: it costs no RAM. The pack is rebuilt only when `content/` or
  `tools/packc` changes. Items without an id in `content/ids.lock` get ids in
  the scratch copy only.
- **Simulator**: `tools/sim/build.sh` compiles the frozen fixture course
  `test/fixtures/content-sim/` into `build/sim/course-sim.pack`; the bundle
  reads it from the host at its first boot. NVS `tinta/sim_pack` names
  another file. Goldens do not move as `content/` grows. The fixture is Units
  0–3, the lexicons they need, the first deck words (with the vulgar
  `pinche`), two phrasebook categories and dictionary-only rows (the vulgar
  `cabrón`, Spain-only `aparcar`, `acera`, `albaricoque`), with its own
  `ids.lock`. `python3 tools/sim/make-fixture.py` regenerates it from
  `content/` (then regenerate the goldens and look at them).

## Input

- Choice bar: a view returns a `ui::ChoiceBar` from `choiceBar()`; the front
  key under cell *i* (or a tap on it) reaches `onAction` as
  `kActionChoice(i)`. Empty or disabled cells fall through.
- Otherwise FreeInkUI focus routing: side keys and Left/Right move focus,
  Confirm activates, Back calls `View::onBack()` (default: pop). `FormView`
  makes Left/Right step the focused value instead when the row has one.
  Screens that draw their own cursor (dictionary, letters, session) return
  -1 from `focusOrdinal()` and take keys in `onInput()`.
- Global: hold Back (0.6 s) or tap the Home pad: pause sheet; hold the Home
  pad: Home; Power: sleep; swipe down from the top (X4 Pro): light sheet.
- `View::onInput()` sees raw events first (the input test takes them all).
- Input that arrives after a screen change waits until the new screen has
  been drawn, so it is routed against the right controls.

## Refresh

Render only when invalidated. A new screen gets `Board::screenRefresh()`: a
half refresh on the X3 (one pass, no ghost of the old screen), a fast refresh
on the X4 family, where the SDK's half refresh is the stock firmware's
flashing clean (on the X4 Classic every card flashed). Changes within a screen
are fast; every Nth screen change (profile `fullRefreshEvery`) and going Home
are full; the first frame after power-on is full. `App::invalidateCard()` is
for content that changes as much as a new screen does without one (the next
card, a dictionary page, a verb tense): a screen refresh, counted toward the
full one every Nth. Within a screen a fast refresh should only add ink, so
a card's front is drawn as the top of its back and the footer shows the
grades greyed out. The simulator's panel model draws anything a non-full
refresh removes as gray 236, so X4-family goldens taken after a screen change
show a faint ghost of the previous screen; the device's differential fast
refresh clears that ink, and the full refresh every Nth change cleans up.
FreeInkApp keeps the strongest hint until the next render; on the S3 boards
frames are drawn while the panel is busy and the newest one is pushed when it
is free. The C3 boards present synchronously. `[tinta] frame <n>` is logged
once the panel has settled, so on the S3 boards a press handled during a
refresh comes before the frame of the press before it: `key left`, `key
confirm`, `screen settings`, `frame 4 fast` (the Left), `frame 5` (Settings).
Flow `15-one-press` checks that each press is one frame.

The X3's first frame after power-on is one half pass: `platform/Board` calls
`skipInitialResync()`, which drops the UC8253 driver's full sync and two
conditioning passes. In the simulator the settled first frame comes 3.2 s
after power-on instead of 6.5 s with the same image; the first visible frame
stays at about 2.9 s there, because the simulator charges the first power-on
command its 1000 ms busy-wait ceiling before the 1600 ms waveform. Whether
the glass shows a ghost of the sleep screen is on the hardware checklist.

## The review session

`SessionController` (one per App) owns a `core::DayQueue` over a 200-entry
array, so a session holds at most 200 items whatever the Session length
setting says. Home's Today card asks it for `todayCounts()`, a dry build of
the whole day's queue (one pass over `items.bin`, cached until a grade, a
setting or the day changes), and Start builds the real one with the profile's
limits: new per day, review cap, session size and `unlockedThrough` (new
items come from the lessons unlocked so far, then the frequency deck). With
"show vulgar words" off, `SessionLimits::excluded` keeps vulgar items out
(`core::session::vulgarItem`).

How each card asks is `core::session::resolveFormat()`: the kind, how well
the item is known (mature, 21 days of stability, means a flashcard), the
device and the typed-answers setting, and the uid and repetition count as
the seed of every random choice, so a resumed or undone card is asked the
same way with the same options. A format the item cannot fill (fewer than
two usable options) falls back to a flashcard. The card line is followed by
`[tinta] ask <format>`.

A self-graded card (flashcards): one item, two presses, two refreshes:

1. The front (`[tinta] card <n>/<total> uid <uid> <kind> front`). Any front
   key, side Down, or a tap on the card reveals: a fast refresh that adds the
   back and turns the greyed grades on, each with its next interval from
   `DayQueue::preview()` (`this session`, `3 d`, `2 mo`, `1.2 y`).
2. A grade (choice bar; side Down is Good): `DayQueue::answer()` writes the
   journal and the item (ProgressStore), then `session.bin` is rewritten in
   place, then the next card is drawn (a screen refresh). `[tinta] review stored
   in <ms>` times the writes.

An auto-graded card (choices, tiles, typing) is answered on its front: the
view turns the answer into a grade (wrong Again; right after a slip, a
mistake or missing accents Hard; right Good), which is journalled at once,
and the same card repaints once with the result (check on the right option,
cross on a wrong choice, the answer and its note): a fast refresh that only
adds ink, except where the front loses ink (the word-order tray, the
keyboard), which is a new screen's refresh. Any key or a tap goes on.

Side Up (swipe right on the X4 Pro) undoes the last grade and shows that card
again: a flashcard with its answer showing, an auto-graded card on its front
with the same options. A grade that makes an item a leech shows a notice with
the item's back and Keep / Suspend before the next card. Hold Back (or the
status bar arrow on touch) opens the pause sheet, which adds End session.
The last grade (or End session) writes the day's totals to `days.bin` and
opens the summary; going to sleep writes the totals so far as well (the
summary still counts the whole session). Answers count at most 60 s each;
simulator builds count 2 s, since their clock races while idle.

If the card stops answering, the grade in flight is lost, the store is
reopened as a guest (RAM only, nothing more is written) and the session goes
on with the banner; the card's files are as they were.

### Exercise views (how a format is added)

`ui::ExerciseView` (`src/ui/views/ExerciseView.h`) draws one card; the
controller and `SessionScreen` do everything else. To add a format:

1. Add it to `core::session::Format` and to `pickFormat()` /
   `resolveFormat()` (`src/core/session/Exercise`), with host tests; add its
   code to `ui::ExerciseFormat` (append only: the code is journalled).
2. Write the view: `accepts(card)` says which formats (and devices) it
   draws; `load(card)` reads what it needs from the pack and returns false
   if it cannot ask the item after all (a flashcard asks it then);
   `draw(screen, area, back)` draws the front, or the back, into `area` and
   nowhere else, with the `CardText` helpers, and registers its touch
   targets (action ids from `kExerciseAction`) on the front.
3. Auto-graded: `autoGraded()` true; answers come as `onChoice(cell)` (from
   `answers()`, a ChoiceBar over the front keys), `onKey()` (raw keys, for a
   cursor of its own and `keyHints()`), or `onAction()` (its touch targets);
   each returns `Answered` with the grade (`core::session::gradeChoice`,
   `gradeTiles`, `gradeTyped`), `Handled` to repaint, or `Ignored`. The back
   is the result; `resultIsNewScreen()` if drawing it takes ink away.
4. Put it ahead of `FlashcardView` and `RevealCardView` in `exerciseViews()`
   (`src/ui/views/Exercises.cpp`): the first view that accepts the format
   asks the item.

| Format | Kind | Asked | Answer |
|---|---|---|---|
| ChooseMeaning | recognise | new and young items | 2–4 English meanings, numbered over the front keys |
| ChooseWord | produce | new and young items | 2–4 Spanish words |
| ChooseGap | cloze | sentence with a gap and its English | 2–4 words for the gap |
| ChooseArticle | gender | the noun without its article | el / la (los / las) |
| ChooseForm | conjugation | infinitive, person and tense | 2–4 forms |
| BuildSentence | word order | the English | the words as tiles: Left/Right and Confirm, or tap |
| TypeWord, TypeGap, TypeForm | produce, cloze, conjugation | X4 Pro, Settings > Study > Type answers | keyboard with an accent row; Check |
| Flashcard | mature items, phrases, a recognise item every third showing after three grades | reveal, four grades | |

Options (`core::session::pickOptions`): the answer and up to three
distractors from the item's ranked candidates (pack format 1.1); authored
partners always among them; dictionary-only, vulgar (with the setting off)
and same-looking candidates skipped; shuffled with the showing's seed.

## Lessons

`LessonScreen` (`src/ui/screens/LessonScreens`) pages through one lesson:
the cover, each note (its spans typeset in their styles: English, bold,
Spanish, respelling, and a not-Mexican word struck through; as many pages as
it needs), the dialogue (whole speaker lines per page; Confirm or the
footer's English shows each line's translation and re-paginates), a
presentation card per new word (`FlashcardView`'s back; vulgar words left
out with the setting off) and the practice page. Start runs
`core::session::lessonPractice()` (the new words' recognise items, then the
lesson's items that need nothing learnt first) as a practice session
(`DayQueue::buildPractice`, tag = lesson + 1) on its own screen stack (Home,
Session). When the practice runs to its end, `App::lessonCompleted()` moves
`profile.currentLesson` past it and `unlockedThrough` up to the next lesson,
and saves the profile at once; the summary says so and offers Next lesson.

Lesson states (`App::lessonState`): done (before `currentLesson`), current,
open (unlocked ahead, by Diagnostics > Unlock all lessons) and locked. Home
shows the current lesson's row; the course map (`CourseScreen`) lists units
and lessons with their state and opens any that is not locked, so a finished
lesson's notes and dialogue can be read and practised again.

## Reader, phrasebook, deck, search, sleep word (M6)

- **Readings** (Home > Read): stories of kind reading, each open once its
  lesson is current, done or unlocked. The reader typesets a paragraph at a
  time into the shared span buffer (`core/reader`), so a page of any length
  costs no more RAM; page starts are a 48-entry array. Every word is a span
  with its story-wide number, and each page keeps where its words were drawn
  (160 boxes): the key cursor walks them, and the X4 Pro's taps are resolved
  against them in `onInput()` (a frame holds only 40 touch targets). The
  cursor is an underline; moving it calls `App::invalidateWindow()` with the
  old and new underlines, which `Board::presentWindow()` refreshes as a window
  of the panel where it can (the X4: logged as `frame <n> window`) and as a
  fast full-frame refresh elsewhere (the X3's drivers have no window; the S3
  boards' async presents would leave the old underline). The gloss box (a screen refresh) shows the word's headword with
  its article, respelling, gloss, its deck state and the English of its
  sentence; on key devices that is how the sentence's English is reached,
  on the X4 Pro the footer's English shows it on its own. Words are linked
  by `Token.lemma`; an unlinked token falls back to a headword or form
  spelled the same (`library::tokenLemma`), and digits have no gloss (the
  cursor skips them). A vulgar word with the setting off shows its
  headword, label and note, as in the dictionary, and no star. After the
  last page the questions (`QuizScreen`); finishing them marks the reading
  read (`App::readLog()`, keyed by a hash of the title so a rebuilt pack
  keeps the marks).
- **Add to my deck**: a star in the reader's gloss and on dictionary entries
  (key devices: Confirm, or the actions sheet for a verb) puts the uid of the
  word's recognise item in `App::starred()` (`starred.bin`, a `MarkLog`).
  Learnt and dictionary-only words cannot be starred; the entry says which.
  The next Today session brings starred words in first, whatever the lessons
  unlocked (`SessionLimits::first`): they need only room in the session and
  then count toward the day's new items. `App::pruneStarred()` drops words no
  longer new when a session starts and ends.
- **Phrasebook** (Home > Phrases): categories, then a category's phrases as
  cards; Practise runs its phrase items as a practice session (tag
  `0x8000 | category`).
- **Search**: on the X4 Pro the dictionary's footer opens `SearchScreen`
  (`core::search::Search`: exact headword, inflected form, prefix, English;
  24 results). On key devices the letter sheet builds a prefix letter by
  letter, the cursor skipping letters that cannot follow, and switches the
  list to English (`EKEY`): at most 25 presses to any headword, 28 to any
  English key, on the full course.
- **Sleep screen**: `App::fillSleepWord()` picks one of the five weakest learnt
  words (`library::pickSleepWord`, never vulgar; a word of the current lesson
  for a new learner), seeded by the day and `profile.sleepCount`, and adds the
  streak and tomorrow's reviews. Settings > Sleep > Plain keeps the plain
  screen. Rotating (the S3 boards, `Power::timerWake()`): the device sleeps
  with the RTC timer set for the next of 8:00, 12:00, 16:00 and 20:00 local
  time; a timer wake (`Power::wokeByTimer()`) draws another word with the
  light off and no screens, writes only the profile's sleep count, and
  sleeps again (`App::rotateSleepWord()`).

## Extending

- Home entry: one line in `kEntries` (`src/ui/screens/HomeScreen.cpp`):
  `{Str::Review, "review", [](App& a) { a.push(ScreenId::Review); }, nullptr, nullptr, nullptr},`
  (the last three: when it is shown, a label that changes, a label written
  at run time).
- Screen: add a `ScreenId` (at the end: ids are stored in `session.bin`),
  derive from `app::View` (or `ui::FormView` for rows), return it from
  `App::view()` in `src/app/Screens.cpp`.
- Choice bar: fill a `ui::ChoiceBar` member and return it from
  `choiceBar()`; handle `kActionChoice` in `onAction()`.
- String: an enumerator in `ui::Str` (`src/ui/Strings.h`) and a row
  `{"English", "Español"}` at the same position in `Strings.cpp`.
- Setting: one `Field` line in the page's table in
  `src/ui/screens/SettingsScreens.cpp`.

## Files on the card

All under `/tinta/`. `tools/progress-dump.py <sd-dir>` prints every one
(`--json`, or `--get journal.count` for one value).

| File | Owner | Written |
|---|---|---|
| `profile.bin` | `core::Profile` | a second after the last settings change, and at once when a lesson is completed, via a temporary file |
| `items.bin`, `reviews.log` | `core::ProgressStore` | every grade, undo and suspend: journal first |
| `days.bin` | `core::DayLog` | when a session ends, and when it is set aside for sleep |
| `session.bin` | `App::saveSession()` | after every grade and on sleep, in place |
| `starred.bin`, `read.bin` | `core::library::MarkLog` | when a word is starred or unstarred, when a reading is finished: 8-byte records appended, compacted now and then |
| `usage-NNNN.log`, `usage.seq` | `core::usage::UsageLog` | after each grade's `session.bin`, at sleep, and when its 1 KB buffer is three quarters full (docs/usage-log.md) |

`session.bin`: magic `TSES`, version 2, the screen stack to restore, then
the review session (`SessionController::serialize()`: the store's journal
count when written, the side showing, totals already in `days.bin`, and the
`DayQueue` blob), CRC-32. Version 1 (M2, stack only) still loads. On boot,
once the day is known, a session from today whose journal count matches comes
back on the same card; one whose journal moved on (a cut between the journal
and `session.bin`) is rebuilt from the store; one from another day is
dropped and the device wakes on the screen below it.

## Simulator

`platform/Board` takes one mutex around the input task's touch poll and the
loop task's battery read: on the X4 Pro both use the I2C bus, and the SDK
simulator's Wire shim keeps a single transaction buffer with no lock, so a
gauge read in the middle of a touch poll garbled the GT911 status into a Home
pad hold nobody pressed (seen only with several simulators running at once).

`TINTA_SIM` (set by `tools/sim/build.sh`) selects: the simulated RTC (NVS
`tinta/sim_clock`, `tinta/sim_clock_run`), the SdFat shim instead of
SDCardManager (and a presence check before reusing an open file, since the
shim keeps writing through one after an eject), the host pack
(`platform/PackImage`), the idle timeout only with `tinta/sim_idle` = 1,
sleeping from a helper task, fixed 2 s answers, and the lines flows drive
by: `[tinta] target <screen>/<row> x y` (touch), `[tinta] row
<screen>/<row> <n>` and `[tinta] focus <screen> <n>` (keys), `[tinta] card
...`, `ask <format>`, `choice answer <n> of <count>`, `tiles <slot>...` and
`tile cursor <n>`, `expect <answer>` and `input <typed>`, targets
`option/<n>`, `tile/<n>`, `key/<label>`, `reveal`, `grade <uid> <g> next
...`, `today day <d> due <n> new <m>`, `lesson <n> page <p>/<count> <kind>
<i>`, `course cursor <lesson> <state>`, `dict top/cursor ...`, `dict row <n>
<word>`, `verb tense <n> <name>`. The flow contract and helpers are in
`tools/sim/test.sh` and `tools/sim/lib.sh` (`answer_card <grade>` answers
any format as a learner meaning that grade). The multi-day flow compares the
firmware with `tools/sim/predict_days.cpp`, the engine and the format picker
on the host, which also seeds cards for the exercise flows (`--learn`: young
items of each kind due on a given day; `--leech`).

## Update from SD card and USB transfer (M7 platform)

Two screens for `m4-session` to build on platform APIs that exist now
(`src/platform/Update.h`, `src/platform/UsbDrive.h`, `src/platform/Version.h`;
the comments there are the contract). Both are device chores, so they live
under Settings. Neither is restorable (`restorable()` false: after the restart
that ends each, the device wakes on Settings), neither allows the global
gestures, and both must keep the device awake while they work (no idle sleep;
Power does nothing). The firmware's identity for About comes from
`platform::version()` ("0.7.0", "0.7.0-draft"), `platform::buildDate()`
("2026-10-04") and `platform::buildDevices()` ("X4 Pro"); `app::kVersion` can
go.

Strings below are the English; they need Spanish rows and the font charset
(PLAN.md 5.4). `<file>` is `update::fileName()` (`tinta-x4-pro.bin`).

### What the screens need from the app

Built (`src/ui/screens/DeviceScreens`): `View::tick()` runs on the top view
every pass of the loop; `View::keepAwake()` skips the idle sleep and Power;
`App::flush()` sets the session's day totals aside and writes `session.bin`
and the profile; `App::paintNow(newScreen)` renders and presents at once,
waiting for the panel, and logs the frame line. Settings lists Update from
SD card after Light, and USB transfer after it where `usbdrive::supported()`;
About shows `version()` and `buildDate()`. The spec as it was asked for:

- A periodic hook on the top view (say `View::tick()`, from `App::loop()`),
  for the USB screen to poll `usbdrive::state()` about every 250 ms and
  invalidate only when it changes.
- A way to flush before handing the card over: the profile if dirty and
  `session.bin` (with the stack below this screen), as `sleepNow()` does,
  without sleeping.
- A way to paint now from inside `update::install()`'s progress callback: the
  loop does not run during the write (30 to 60 s), so the callback renders the
  top view and presents it itself (`Refresh::Fast` for the bar, waiting on
  `refreshBusy()` on the S3), at most every 10 %.
- A view-level "keep awake" (the idle timeout and Power both skipped).
- Stopping after a restart request: `update::restart()` and `usbdrive::end()`
  go through `platform::restart()` (`Power.h`), which never returns on the
  device but returns at once in simulator bundles (the loop task must unwind
  for the bundle to be restarted, as with `Power::sleep()`). The caller returns
  from its handler, and `App::loop()` and `App::setup()` (right after
  `board_.begin()`: the update combo can restart from there) return without
  drawing while `platform::restartRequested()`.

### Update from SD card (all devices)

Settings > **Update from SD card**. On entry: `update::find(app.storage())`
(reads about 400 bytes; instant).

| State | Body | Keys / touch |
|---|---|---|
| `NoCard` | No SD card. | Back |
| `NoFile` | No update on the card. Copy `<file>` to the top of the card (not into a folder) and open this again. / Installed: Tinta `version()` (`buildDate()`). | Back |
| `NotFirmware` | `<file>` is not a Tinta firmware. | Back |
| `WrongChip`, `WrongDevice` | `<file>` is Tinta for another device. This is an `buildDevices()`: use `<file>` from the release's sd-card folder. | Back |
| `TooLarge` | `<file>` is too large for this device. | Back |
| `ReadError` | The SD card could not be read. | Back |
| `Ready` | On the card: Tinta `image.version` (`image.date`). Installed: `version()`. (If `image.sameVersion`: This version is already installed.) Installing takes about a minute and does not touch your progress. | Back, **Install** |
| Ready, battery under 20 % and not charging | as Ready, plus: Charge the battery first. Install greyed out. | Back |
| Installing, `Phase::Checking` | Checking the file… Do not turn the device off. | none |
| Installing, `Phase::Writing` | Installing Tinta `image.version`… a bar and the percent (`done * 100 / total`). If it turns off, this version starts as before and you can try again. | none |
| `Installed` | Tinta `image.version` installed. Restarting… (present, wait for the refresh, then `update::restart()`) | none |
| `Damaged` | The file is damaged or incomplete. Copy `<file>` again. Nothing was changed. | Back |
| `WriteError` | Installing failed. Tinta `version()` is still installed. | Back |

Install: flush (above), then `update::install(app.storage(), progress, this)`
(it closes Storage's file, checks the file again, writes it, renames it
`<file>.flashed`). Any status but `Installed` shows its row above.

If `update::bootComboResult()` is not `None` at boot (the update combo was
held, nothing was installed), open this screen once over the first screen with
that status, so holding the keys is never silent. Combo, for help text:
`update::combo(board)`: Back + Power on the X3, X4 and X4 Classic, Down + Power
on the X4 Pro.

### USB transfer (X4 Classic, X4 Pro)

Settings > **USB transfer**, shown only when `usbdrive::supported()`.

| State | Body | Keys / touch |
|---|---|---|
| intro (before `begin()`) | Use the SD card from a computer: copy your progress (the tinta folder) off or back, or put an update on the card. Tinta cannot use the card meanwhile; the device restarts when you are done. | Back, **Start** |
| `Waiting` | Connect the USB cable to a computer. | **Cancel** (= `end()`) |
| `Connected` | Connected. The card is a drive on your computer. Eject it there before unplugging the cable. | none |
| `Ejected` | Ejected. Restarting… then `end()` | none |
| `Unplugged` | Unplugged without ejecting. Restarting… If files were being copied, check them. then `end()` | none |
| `Error` | USB transfer stopped: the card could not be used. / (from a failed `begin()`) USB transfer could not start. | Back (= `end()`) |

Start: flush (above), then `usbdrive::begin(app.storage())`; false means
`Error`. From `begin()` on, only `usbdrive::end()` leaves (it restarts), and
the store is detached (`Storage::available()` false): nothing may write. Poll
`usbdrive::state()`; on `Ejected` or `Unplugged` show the row for one refresh,
then `end()`. `Waiting` for 5 minutes: `end()`. The serial log is gone from
`begin()` until the restart (the S3's USB port is the drive meanwhile).

### Simulator

- Update: put an image on the bundle's card directory:
  `python3 tools/release/fake-image.py --build x4-pro -o <sd>/tinta-x4-pro.bin`
  (`--chip c3` wrong chip, `--build x4-classic` wrong device, `--no-tag` not
  Tinta, `--truncate 40000` damaged; default version 9.9.9). The header checks
  and the segment walk are real; writing is simulated (4 KB steps) and NVS
  `tinta/sim_update` picks the outcome: empty (installed), `damaged`,
  `write-error` (at 50 %), `read-error`. `update::restart()` restarts the
  bundle; the file is then `<file>.flashed`. Bundles report version
  `0.0.0-sim`, built `2026-10-03`.
- USB (X4CLASSIC, X4PRO; `supported()` false on X3 and X4): `begin()` detaches
  Storage as on the device; then `sim nvs set tinta sim_usb connected` (or
  `ejected`, `unplugged`, `error`; empty is `waiting`) while the screen polls.
  `fail` before Start makes `begin()` fail. `end()` restarts the bundle.
- Lines to sync on: `[tinta] update find <status> [<version>]`, `update
  installing <file> <version> (<bytes> bytes)`, `update writing <n>%` (each
  10 %), `update installed <version>`, `update failed: <status>`, `update
  restart`, `update combo held: <file> <status>`; `[tinta] usb <state>`, `usb
  end, restart`, `storage: card handed over until restart`. Status and state
  names are `update::statusName()` and `usbdrive::stateName()`: `ready`,
  `no-file`, `wrong-chip`, ...; `waiting`, `connected`, ...

## First run, interface language, About (M7 interface)

- **First run** (`src/ui/screens/WelcomeScreens`): with a card and no
  `profile.bin`, the boot goes to `welcome` instead of Home or the time step.
  It asks the interface language in both languages (English, Español, Auto);
  then, if the clock is not trusted (the X4 always, an RTC that was never
  set), `App::beginTimeStep({welcome, key-guide})` runs the date picker or
  the clock screen and comes back to `key-guide`: how the four keys, the side
  keys, holding Back and Power work, or on the X4 Pro tapping, the card, the
  Home pad, the light and Power. Next goes to `vulgar-choice` (the "show
  vulgar words" toggle, off, with what it hides), whose Start goes Home. Back
  steps back through the three; they are restorable, so a sleep halfway
  comes back to the same page. Choosing the language saves the profile, so
  the next boot is not a first run. A guest (no card) skips it, since nothing
  would be kept. Logs: `screen welcome`, `set uiLanguage <english|spanish|auto>`,
  `screen key-guide`, `screen vulgar-choice`, `set showVulgar <on|off>`.
- **Interface language**: `App::applyLanguage()` resolves the profile's
  setting into `ui::setLanguage()`, which only knows English and Spanish.
  Auto is English until the current lesson is past Unit 4 (or the course is
  finished), then Spanish (PLAN.md 4.1); Settings shows it as "Auto
  (English)" or "Auto (Spanish)". `ui::trIn()` gives a string in a given
  language (the welcome page shows both). The X4 Pro keyboard's delete and
  space keys follow the language; flows still find them as `key/del` and
  `key/space`. Not translated, on purpose: the input test and the type
  specimen (Diagnostics), and the legal text on the licence pages.
  Every footer label measures at most 115 px in Helvetica Bold 20 in either
  language (the 480-px devices' cells leave 116).
- **About** is a form: the name, version, device, panel and course, then
  `keys` (the key guide again; there it only goes back) and `licences`: five
  pages from `NOTICE` (Times and Helvetica with Adobe's permission notice;
  the 22/29 px CrossPoint strikes and TeX Gyre Termes under the GUST Font
  License; the software as `NOTICE` lists it: the FreeInk SDK and the OpenX4
  community SDK, the py-fsrs port, SdFat, the Arduino core (LGPL) and ESP-IDF
  with FreeRTOS, newlib, mbedTLS and TinyUSB; the MIT License; the course's
  sources), turned with
  Left/Right or the side keys, or the footer arrows and swipes on the X4 Pro.
  Logs `licences page <n>/5`.
- **Flashcard front hint**: the first self-graded card of a session
  (flashcard or reveal card; `SessionController::showHint()`) says "Press any
  key to show the answer" ("Tap to show the answer" on the X4 Pro) on its
  last line, in the style of a result's "Press any key to go on". The back
  draws over that line, so that one reveal is a screen refresh
  (`invalidateCard()`); every other front has no hint and its reveal stays
  fast. A session resumed after a wake does not show it again.

## Usage log call sites (M7)

The engine is `core::usage::UsageLog` (docs/usage-log.md); `App::usage()`
reaches it. Writes: `flush()` after the journal write's `session.bin` in
`SessionController::grade()`, `answer()`, `undo()` and a suspend; in
`App::sleepNow()`, `lowBatterySleep()` and the rotation sleep after the
`sleep` record; in `App::flush()` (update, USB); and from `App::loop()` when
`needsFlush()` and no frame is pending or on its way to the panel. Never from
a build.

Records, by where they are made:

- `App`: `boot` and `battery` in `startUsageLog()` (after the battery read;
  the pack, progress-store and card errors follow it); `screen` in
  `transition()` (push 0, pop 1, reset 2, restored 3); `input` after
  `dispatch()` for every event: `ignored` when nothing on screen changed
  (`changes_` counts every invalidation, screen change and sleep request),
  `queued` when it was handled while the last frame was still going to the
  panel; `frame` when a frame has settled, with the time since the first
  press that changed something since the frame before (handling time, not
  the electrical press); `star` in `toggleStar()`; `clock_change` for a
  rollover in `pollPeriodic()`; `error` for a card failure, a pack error, a
  guest store and a saved session that did not come back.
- `SessionController`: `session_start` when a session begins, and with
  `resumed` set when one comes back (or is rebuilt) after a wake; `session_end` finished / ended (pause sheet) /
  left (another practice replaced it) / slept (`setAside()`, totals so far);
  `item_shown` each time a card is shown, with a choice's option texts
  (`ExerciseView::optionTexts()`); `answer` from the grade and
  `ExerciseView::answerDetail()` (option, typed text, wrong tiles): an
  auto-graded Again is wrong, Hard near, Good right; `reveal`, `undo`.
- Screens: `lesson_page` and `dialogue_english` (LessonScreen);
  `story_open` (once paginated), `story_page`, `gloss` (when the box
  opens), `sentence_english`, `quiz_answer`, `story_done` (Reader, Quiz);
  `search` (SearchScreen: the query when the learner opens a result or
  leaves, not every letter; the letter sheet: each prefix, `letter` or
  `english`); `entry` (`showEntry()`, via dictionary or search);
  `verb_table` per tense shown; `phrasebook` opened and practised;
  `setting` for every Settings field (`Field::number`) and the first run's
  two choices; `clock_change` set (SetClock), day-confirmed (X4),
  time-zone (Settings).

Settings > Study > Record usage is not there yet: it needs a profile field,
and until then the log is always on (with a card).

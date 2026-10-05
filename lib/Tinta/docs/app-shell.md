# App shell

How Tinta's app around the course is put together inside lila, and how to
extend it. PLAN.md sections 4.4–4.7, 6.4–6.8 and 8 are the design; this is the
code. Paths are under `lib/Tinta/` unless they start with `src/activities/`.

## Pieces

| Where | What |
|---|---|
| `src/activities/tinta/TintaActivity` | The lila activity: allocates the `App` in `onEnter()` and frees it in `onExit()`; input and updates on lila's loop task, frames on its render task; the sleep card; lila's light panel on request |
| `src/activities/tinta/platform/` | Tinta's platform layer over lila's HAL: Board (input queue, present, battery), Clock, StateFiles, PackFile, Power, Log |
| `src/app/App` | Opening and closing, the course and progress, the screen stack, input dispatch, refresh policy, profile and session saving, the sleep card |
| `src/app/SessionController` | The review session (Today, or a lesson's practice): queue, format per card, card and side, grades, undo, leeches, day totals, lesson completion, its part of `session.bin` |
| `src/app/View.h` | The screen interface, `ScreenId`, shared action ids |
| `src/app/Screens.cpp` | One instance of each screen, by `ScreenId` |
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
| `src/core/library/` | Lookups: readings, story keys, a lemma's recognise item, a category's phrases, a word's lemma; `MarkLog` (starred words, readings read); the sleep card's word |
| `src/core/reader/` | A story as typesetter spans, a paragraph at a time, every word numbered |
| `src/core/search/` | Dictionary search (headword, inflected form, prefix, English) and the next letters of a prefix |
| `src/core/session/` | Format picking, options, word-order tiles, grades from answers, a lesson's practice list (pure, host-tested) |
| `src/core/lang/AnswerCheck` | Typed answers: case, accents, one slip, non-Mexican synonyms |
| `src/ui/views/CardText` | Typeset blocks for cards and the dictionary: label line, headword, sentence with the word in bold, labelled values |
| `src/ui/screens/*` | Home, Session and Summary, Lesson and Course map, Readings, Reader and Questions, Phrasebook, Progress, Dictionary (list, letters, entry, entry actions, verb table, search), Settings, the first run (`WelcomeScreens`), About and Licences, Diagnostics, pack error, sheets, the date prompt and picker, the sleep card |

## Opening

`App::open()`: the screens and shared buffers first (the largest blocks,
while the heap is least broken up), the card and the profile; then the course
(`openCourse()`): `/tinta/course.pack` is opened through `platform::PackFile`
(an 8 × 1 KB block cache) and `Pack::open()` runs the structural checks only
(`verifyCrc()` reads the whole pack and runs from Diagnostics > Check course
data). The FSRS model is configured from the profile, the slot table is
allocated (2 bytes per pack item: the only allocation that grows with the
course) and `ProgressStore::open()` replays or rebuilds what it must (logged
as `[tinta] progress <result> in <ms>`). Then the clock (PLAN.md 6.8): without
a trusted one, the date is asked first. Without a pack the root screen is the
pack error screen instead of Home (`App::rootId()`).

`App::close()`, from `TintaActivity::onExit()` before lila sleeps or opens
anything else, leaves the top view, writes `session.bin`, the profile if it
changed and the usage log, and frees the course. Sleeping from inside Tinta
calls `App::drawSleepCard()` first (lila's `Activity::drawSleepFrame()`).

## The course pack

`/tinta/course.pack` on the card, compiled by `tools/packc` from `content/`
(docs/tinta.md in lila). Pack strings are copied into a 6 KB arena that
`Pack::beginPass()` resets at each event and frame, so a string from the pack
is valid until the next one (`core/pack/PackSource.h`).

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
  pad: Home. Power and sleep are lila's; so is the light panel (swipe down
  from the top on the X4 Pro, or Light in the pause sheet or Settings, which
  ask `TintaActivity` for it through `App::takeHostRequest()`).
- `View::onInput()` sees raw events first.
- Input that arrives after a screen change waits until the new screen has
  been drawn, so it is routed against the right controls.

## Refresh

Render only when invalidated. A new screen gets `Board::screenRefresh()`: a
half refresh on the X3 (one pass, no ghost of the old screen), a fast refresh
on the X4 family, where a half refresh is the flashing clean waveform.
Changes within a screen are fast; every Nth screen change (profile
`fullRefreshEvery`), going Home and the first frame after Tinta opens are
full. `App::invalidateCard()` is for content that changes as much as a new
screen does without one (the next card, a dictionary page, a verb tense): a
screen refresh, counted toward the full one every Nth. Within a screen a fast
refresh should only add ink, so a card's front is drawn as the top of its back
and the footer shows the grades greyed out. FreeInkApp keeps the strongest
hint until the next render. A frame something else drew over (lila's light
panel) is repainted whole with a screen refresh. `[tinta] frame <n>` is logged
once the panel has settled.

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
opens the summary; closing Tinta writes the totals so far as well (the
summary still counts the whole session). Answers count at most 60 s each.

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

## Reader, phrasebook, deck, search, sleep card (M6)

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
  fast full-frame refresh elsewhere. The gloss box (a screen refresh) shows
  the word's headword with
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
- **Sleep card**: with lila's Current Page sleep screen,
  `App::drawSleepCard()` picks one of the five weakest learnt words
  (`library::pickSleepWord`, never vulgar; a word of the current lesson for a
  new learner), seeded by the day and `profile.sleepCount`, and draws it with
  the streak and tomorrow's reviews (`ui/screens/SleepScreen`) into the
  framebuffer; lila's sleep screen adds its moon and puts it up with a half
  refresh. Tinta's own sleep settings (Plain, Rotating) are not used in lila.

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
| `days.bin` | `core::DayLog` | when a session ends, and when it is set aside as Tinta closes |
| `session.bin` | `App::saveSession()` | after every grade and when Tinta closes, in place |
| `starred.bin`, `read.bin` | `core::library::MarkLog` | when a word is starred or unstarred, when a reading is finished: 8-byte records appended, compacted now and then |
| `usage-NNNN.log`, `usage.seq` | `core::usage::UsageLog` | after each grade's `session.bin`, when Tinta closes, and when its 1 KB buffer is three quarters full (docs/usage-log.md) |

`session.bin`: magic `TSES`, version 2, the screen stack to restore, then
the review session (`SessionController::serialize()`: the store's journal
count when written, the side showing, totals already in `days.bin`, and the
`DayQueue` blob), CRC-32. Version 1 (stack only) still loads. When Tinta
opens, once the day is known, a session from today whose journal count
matches comes back on the same card; one whose journal moved on (a cut
between the journal and `session.bin`) is rebuilt from the store; one from
another day is dropped and Tinta opens on the screen below it.

## First run, interface language, About

- **First run** (`src/ui/screens/WelcomeScreens`): with a card and no
  `profile.bin`, Tinta opens on `welcome` instead of Home or the time step.
  It asks the interface language in both languages (English, Español, Auto);
  then, if the clock is not trusted (the X4 always, an RTC that is not set),
  `App::beginTimeStep({welcome, key-guide})` runs the date picker and comes
  back to `key-guide`: how the four keys, the side
  keys, holding Back and Power work, or on the X4 Pro tapping, the card, the
  Home pad, the light and Power. Next goes to `vulgar-choice` (the "show
  vulgar words" toggle, off, with what it hides), whose Start goes Home. Back
  steps back through the three; they are restorable, so a sleep halfway
  comes back to the same page. Choosing the language saves the profile, so
  the next opening is not a first run. A guest (no card) skips it, since nothing
  would be kept. Logs: `screen welcome`, `set uiLanguage <english|spanish|auto>`,
  `screen key-guide`, `screen vulgar-choice`, `set showVulgar <on|off>`.
- **Interface language**: `App::applyLanguage()` resolves the profile's
  setting into `ui::setLanguage()`, which only knows English and Spanish.
  Auto is English until the current lesson is past Unit 4 (or the course is
  finished), then Spanish (PLAN.md 4.1); Settings shows it as "Auto
  (English)" or "Auto (Spanish)". `ui::trIn()` gives a string in a given
  language (the welcome page shows both). The X4 Pro keyboard's delete and
  space keys follow the language; flows still find them as `key/del` and
  `key/space`. Not translated, on purpose: the legal text on the licence
  pages.
  Every footer label measures at most 115 px in Helvetica Bold 20 in either
  language (the 480-px devices' cells leave 116).
- **About** is a form: the name, lila's version, device, panel and course, then
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

## Usage log call sites

The engine is `core::usage::UsageLog` (docs/usage-log.md); `App::usage()`
reaches it. Writes: `flush()` after the journal write's `session.bin` in
`SessionController::grade()`, `answer()`, `undo()` and a suspend; in
`App::flush()` (when Tinta closes); and from `App::update()` when
`needsFlush()` and no frame is pending. Never from a build.

Records, by where they are made:

- `App`: `boot` in `startUsageLog()` (each opening; the pack, progress-store
  and card errors follow it); `screen` in `transition()` (push 0, pop 1,
  reset 2, restored 3); `input` after `dispatch()` for every event: `ignored`
  when nothing on screen changed (`changes_` counts every invalidation and
  screen change), `queued` when it was handled while the last frame was still
  going to the panel; `star` in `toggleStar()`; `clock_change` for a rollover
  in `pollPeriodic()`; `error` for a card failure, a pack error, a guest store
  and a saved session that did not come back.
- `SessionController`: `session_start` when a session begins, and with
  `resumed` set when one comes back (or is rebuilt) as Tinta reopens;
  `session_end` finished / ended (pause sheet) /
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
  two choices; `clock_change` day-confirmed (the date prompt and picker).

There is no Record usage setting yet; the log is always on (with a card).

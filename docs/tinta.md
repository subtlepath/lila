# Learn Spanish (Tinta)

**Learn Spanish** in the Home Menu is Tinta, an offline Mexican Spanish course: lessons, a daily review session
with spaced repetition, graded readings, a phrasebook and a dictionary with verb tables. It is the same app as the
standalone Tinta firmware, running inside lila.

Available on the X3, X4, X4 Classic and X4 Pro.

## Setting it up

Tinta keeps everything in one folder on the SD card, `/tinta/`:

| File | What it is |
|---|---|
| `course.pack` | The course: lessons, readings, phrasebook, dictionary. About 3 MB. Required. |
| `profile.bin`, `items.bin`, `reviews.log`, `days.bin`, `session.bin`, `starred.bin`, `read.bin`, `usage-*.log` | Your settings and progress. Tinta creates them. |

1. Download `course.pack` from the lila release and copy it to `/tinta/course.pack` on the card.
2. Open **Home Menu → Learn Spanish**.

Without `course.pack`, Tinta opens on a page saying the course could not be read.

The folder is the one the standalone Tinta firmware uses. A card that has been in a device running Tinta keeps its
progress when the device switches to lila, and the other way round. To back up your progress, copy the `/tinta/`
folder.

### Building `course.pack`

The course is compiled from its sources in `lib/Tinta/content` (needs Python 3 and PyYAML):

```sh
cd lib/Tinta
python3 tools/packc --out build/course.pack
```

A new `course.pack` can replace the old one at any time. Progress is kept by item, so it carries over to a new
edition of the course. A review session paused when the pack changed is dropped if its lesson or phrase category no
longer exists.

## Using it

- **Back** goes back a screen. On Tinta's Home it returns to lila; so does **Leave** on Tinta's Home and in the pause
  sheet.
- **Hold Back** opens the pause sheet (on the X4 Pro, tap the Home pad).
- Tinta's footer labels the four front buttons by position. Button remapping in lila's settings does not apply inside
  Tinta.
- **Power** and auto-sleep work as everywhere in lila. Tinta saves your place first, and opening it again continues
  where you were, in the middle of a session too.
- With lila's sleep screen on **Current Page**, sleeping from Tinta leaves a word card on the screen instead of the
  page you were on: one of your weakest words, with its pronunciation, meaning and an example, under the date, with
  your streak and tomorrow's reviews. Each sleep shows another word. Before you have learnt any, it shows a word from
  your current lesson. The other sleep screens work as everywhere in lila.
- Tinta's day follows lila's clock and time zone (**Settings › Clock**). On the X4, which has no clock, or where the
  clock has not been set, Tinta asks for the date the first time it is opened after the device starts.
- Tinta's own Settings cover study (new words per day, retention, session length), display (text size, interface
  language, how often the screen fully refreshes) and the date: the hour a study day starts, or, without a clock,
  today's date. Sleep, the frontlight, the clock and its time zone, firmware updates and USB belong to lila.

## How it fits in lila (for developers)

- Code: `lib/Tinta/src` (`core/` is the engine, `app/` and `ui/` the screens) and `src/activities/tinta/`
  (`TintaActivity` and the platform layer over lila's HAL). Build flag `LILA_TINTA`, on for the C3, X4 Classic and
  X4 Pro environments.
- Course: `lib/Tinta/content` (the sources), compiled by `lib/Tinta/tools/packc`. `lib/Tinta/docs/content-style.md` is
  the authoring guide and `lib/Tinta/docs/pack-format.md` the file format. A normal `packc` run appends new item ids
  to `content/ids.lock`; commit it with the content, since progress is stored by those ids. Releases attach
  `course.pack`, built with the plain check (`packc --release` also requires every lesson to be marked reviewed).
- Fonts: `lib/Tinta/src/fonts` is generated from lila's own Times and Helvetica in `x11/` (and TeX Gyre Termes for
  the headwords) by `sh lib/Tinta/tools/gen-fonts.sh`, after a glyph change there; `check-fonts.sh` reports a stale
  strike. Icons: `python3 lib/Tinta/tools/gen-icons.py`.
- The course pack is read from the card through a block cache (8 × 1 KB) and a 6 KB per-pass string arena
  (`core/pack/PackSource.h`, `Pack::beginPass()`): a string from the pack is valid until the next event or frame.
- Everything Tinta allocates (screens, text buffers, the slot table, the pack cache; about 60 KB) exists only between
  `TintaActivity::onEnter()` and `onExit()`. A closed Tinta keeps about 220 bytes of static RAM.
- Host tests: `sh test/tinta/run.sh`, also part of `ctest`. Each runs twice, with the pack in memory and through the SD
  cache (`TINTA_PACK_SOURCE=file`). `sh test/tinta/make-fixtures.sh` recompiles the committed fixtures after a change
  to the compiler, the charset or a fixture course; with `--full` it also builds `fixtures/full.pack`, the whole
  course, which is not committed (the tests that need it are skipped without it).
- Tool tests: `python3 -m unittest discover -s packc/tests -t .` (compiler) and `python3 -m unittest discover -s
  tests` (fonts, usage report), both from `lib/Tinta/tools`.
- A card's `/tinta/` folder can be read on a computer: `python3 lib/Tinta/tools/progress-dump.py SD` prints the
  progress files and `python3 lib/Tinta/tools/usage-report.py SD` reports on the usage logs
  (`lib/Tinta/docs/usage-log.md`), where `SD` is the card's root.
- Serial log lines are tagged `TNT`. Opening logs the heap before and after; closing logs the heap, the render task's
  stack low-water mark, and the arena's peak.

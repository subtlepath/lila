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

1. Copy `course.pack` to `/tinta/course.pack` on the card.
2. Open **Home Menu → Learn Spanish**.

Without `course.pack`, Tinta opens on a page saying the course could not be read.

The folder is the one the standalone Tinta firmware uses. A card that has been in a device running Tinta keeps its
progress when the device switches to lila, and the other way round. To back up your progress, copy the `/tinta/`
folder.

### Building `course.pack`

The course is compiled from the Tinta sources (`../spanish`):

```sh
cd ../spanish
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
- On the X4, which has no clock, Tinta asks for the date the first time it is opened after the device starts.
- Tinta's own Settings cover study (new words per day, retention, session length), display (text size, interface
  language, how often the screen fully refreshes) and date and time. Sleep, the frontlight, firmware updates and USB
  belong to lila.

## How it fits in lila (for developers)

- Code: `lib/Tinta/src` (copied from the Tinta repository: `core/` is the engine, `app/` and `ui/` the screens) and
  `src/activities/tinta/` (`TintaActivity` and the platform layer over lila's HAL). Build flag `LILA_TINTA`, on for the
  C3, X4 Classic and X4 Pro environments.
- The course pack is read from the card through a block cache (8 × 1 KB) and a 6 KB per-pass string arena
  (`core/pack/PackSource.h`, `Pack::beginPass()`): a string from the pack is valid until the next event or frame.
- Everything Tinta allocates (screens, text buffers, the slot table, the pack cache; about 60 KB) exists only between
  `TintaActivity::onEnter()` and `onExit()`. A closed Tinta keeps about 220 bytes of static RAM.
- Host tests: `sh test/tinta/run.sh`, also part of `ctest`. Each runs twice, with the pack in memory and through the SD
  cache (`TINTA_PACK_SOURCE=file`). `fixtures/full.pack`, the whole course, is optional and not committed; build it
  with the command in `test/tinta/run.sh`.
- Serial log lines are tagged `TNT`. Opening logs the heap before and after; closing logs the heap, the render task's
  stack low-water mark, and the arena's peak.

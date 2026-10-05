# Tinta

The Spanish course app behind **Learn Spanish** (docs/tinta.md), copied from the standalone Tinta firmware
(`../spanish`, 2026-10-04) and adapted to run inside lila. The two copies are maintained separately.

- `src/core`: the engine (course pack reader, spaced repetition, sessions, search, typesetting). Freestanding
  C++17; host tests in `test/tinta`.
- `src/app`, `src/ui`: the screens, drawn with FreeInkUI into lila's framebuffer.
- `src/fonts`, `src/icons`: Tinta's bitmap strikes and icons.
- `src/platform/*.h`: what Tinta needs from the device. lila implements it in `src/activities/tinta/platform`.

Changes from the standalone firmware: the course pack is read from `/tinta/course.pack` through `PackSource`
instead of being linked into the image; screens and buffers are allocated while Tinta is open; lila owns power,
sleep, the frontlight, firmware updates and USB, so those screens are gone; Back on Tinta's Home returns to lila.

Font and course notices: `NOTICE`, also shown in Tinta's Settings → About → Licences.

# Tinta — a Mexican Spanish tutor for Xteink e-paper readers

The design of Tinta, the Spanish course that lila opens as **Learn Spanish**
(`docs/tinta.md` in lila is the user-facing page). *Tinta* means "ink".

Tinta began as a standalone firmware on the FreeInk SDK and now lives in lila:
the engine and screens in `lib/Tinta/src`, hosted by
`src/activities/tinta/TintaActivity`. Sleep, power, the frontlight, USB,
firmware updates and the clock's time zone are lila's. Section 11 says what is
built and what is open. Section numbers are cited from the code, so they stay
put.

---

## 1. Summary

Tinta teaches English speakers the Spanish spoken in Mexico: Mexican
vocabulary, `ustedes` instead of `vosotros`, Mexican pronunciation and everyday
register. It runs offline on the X3, X4, X4 Classic and X4 Pro.

| Decision | Choice | Why |
|---|---|---|
| Host | A lila activity (`LILA_TINTA`, on for the C3, X4 Classic and X4 Pro builds) | One firmware for reading and the course |
| UI stack | FreeInkUI (`FreeInkApp` + `DisplayTarget`) drawing into lila's framebuffer | No external graphics library; screens run in host tests |
| Fonts | lila's X11 BDF strikes, converted to FreeInkUI `BitmapFont` data | The BDF is 1-bit, so conversion is lossless; a Spanish/English subset costs 3–11 KB per strike |
| Typeface roles | **Times = Spanish, Helvetica = English and chrome** | The typeface tells the learner which language they are reading |
| Content | One binary pack, `/tinta/course.pack` on the SD card, read through a block cache | No flash cost; a new edition is a file copy |
| Linguistics | Done at build time in Python; the device only reads tables | Conjugation, respelling and tokenising are testable off-device |
| Scheduling | FSRS spaced repetition, four grades | Four grades map one-to-one onto the four front keys of the X3, X4 and X4 Classic |
| Progress | SD card: fixed-size state file plus append-only review journal | Survives power loss; the learner can back it up |
| Audio | None | None of the four devices has audio; pronunciation is taught with respelling |

Five principles drive the design:

1. **One press, one refresh.** Every answer is a single key press or tap. No
   focus-moving that costs a panel refresh before the answer registers.
2. **Smart compiler, dumb device.** Anything linguistic happens in the content
   compiler. The firmware does table lookups over a fixed binary layout.
3. **Same lesson on every device.** Every exercise has a four-key form (X3, X4,
   X4 Classic) and a touch form (X4 Pro).
4. **Never lose progress.** Each review is journaled before the screen changes.
5. **Mexican by default.** Content is linted for Peninsular forms.

---

## 2. Goals and non-goals

### Goals for v1

- A structured A1–A2 course: 13 units, 44 lessons, about 1,500 lemmas.
- Daily spaced-repetition review in sessions of 5–10 minutes.
- Seven exercise types (section 4.3), all usable with keys only.
- Graded dialogues and short readings with word glossing.
- Phrasebook for travel situations in Mexico.
- Spanish–English dictionary of about 5,000 headwords with verb tables.
- Streak, statistics and a sleep card that shows a word to learn.

### Non-goals for v1

- Audio, speech or listening exercises (no hardware for it).
- Accounts or syncing progress over Wi-Fi.
- European Spanish, voseo, or other regional variants.
- Landscape orientation.

### Later (section 11)

Typed answers with a BLE keyboard on key devices, SD expansion packs, Anki
import, B1 content.

---

## 3. Hardware targets

Four devices in two MCU families. "Key devices" in this plan means the X3, X4
and X4 Classic; the X4 Pro is the only touch device.

| | X3 | X4 | X4 Classic | X4 Pro |
|---|---|---|---|---|
| MCU | ESP32-C3 | ESP32-C3 | ESP32-S3 | ESP32-S3 |
| Portrait logical size | 528×792 | 480×800 | 480×800 | 480×800 |
| Input | 4 front keys, 2 side keys, Power | same as X3 | 4 bottom keys, 2 side keys, Power | GT911 touch, Home pad, 2 side keys, Power |
| Frontlight | none | none | none | warm/cold PWM (lila's light panel) |
| RTC | DS3231 | **none** | BM8563 | BM8563 |

Consequences:

- **Design to the C3.** RAM budgets and refresh behaviour are set by the X3 and
  X4, with about 380 KB of RAM and no PSRAM (section 6.6).
- **Two input models, two screen sizes.** The three key devices expose the same
  seven logical keys; the X4 Pro is touch-first with two keys. Layouts adapt
  to 528×792 and 480×800. Section 4.4 defines one semantic mapping.
- **On the C3 boards the SD card shares the display SPI bus.** Tinta's SD reads
  and writes go through lila's `HalStorage`, on the loop task, never during a
  present.
- **The X4 has no clock.** Section 6.8 defines how scheduling works on it.

---

## 4. Product design

### 4.1 Learning model

- **Lessons introduce, spaced repetition retains.** A lesson presents about ten
  new items with a short grammar or culture note and a dialogue. From then on
  the scheduler decides when each item returns.
- **Items, not cards.** The schedulable unit is an *item*: `(lemma, recognise)`,
  `(lemma, produce)`, a cloze sentence, a conjugation, a gender, a phrase. The
  exercise *format* for an item is chosen at review time, so the same word
  comes back as a flashcard one day and inside a sentence the next.
- **Recognition before production.** The produce item for a word unlocks when
  its recognise item reaches about three days of stability.
- **Words live in sentences.** Every lemma has at least one example sentence;
  cloze items are generated from those sentences.
- **Short sessions.** The Today session is due reviews, then new items up to
  the daily limit (default 10), capped at about 40 items.
- **Immersion ramp.** Chrome starts in English. A setting switches labels to
  Spanish (`Repaso`, `Otra vez`, `Bien`); "Auto" switches after Unit 4.

### 4.2 What makes it Mexican

| Area | Rule in the content |
|---|---|
| Pronouns | `ustedes` for plural "you". No `vosotros` anywhere; verb tables have five rows |
| Vocabulary | `computadora`, `celular`, `jugo`, `papa`, `alberca`, `camión` (bus), `chamarra`, `lentes`, `departamento`, `manejar`, `platicar`, `popote`, `banqueta`, `boleto` |
| Hazards | Words that differ in register, for example `coger` is vulgar in Mexico; teach `tomar` and `agarrar` |
| Courtesy | `¿mande?`, `con permiso`, `provecho`, `usted` with strangers and elders, softening diminutives (`ahorita`, `un momentito`) |
| Tense preference | Preterite for completed past where Spain uses the present perfect |
| Colloquial | `órale`, `ándale`, `híjole`, `¿qué onda?`, `chido`, `padre`, `chamba`, `lana`, `neta`, `no manches`, each with a register tag |
| Nahuatl loans | `aguacate`, `jitomate`, `elote`, `cuate`, `papalote`, `tianguis`, `apapachar` |
| Pronunciation | Seseo (`z`, `ce`, `ci` as *s*), yeísmo, the four readings of `x` (`México`, `Xochimilco`, `Xola`, `taxi`), `tl` |

Each lemma carries a register tag (formal, neutral, informal, vulgar) and a
"Mexico-specific" flag. Vulgar items are hidden unless enabled in settings.
When a learner types a correct non-Mexican synonym, it is accepted and the
Mexican word is shown.

**Pronunciation without audio.** Each lemma and phrase has an English-based
respelling with the stressed syllable in capitals: `chamba` → `CHAHM-bah`,
`gracias` → `GRAH-syahs`. IPA is not used: the Times and Helvetica strikes lack
most IPA letters (`ɲ ʝ ɾ ɣ ʃ` are absent). Unit 0 teaches the sound system.

### 4.3 Exercise types

| # | Exercise | Tests | Key devices | X4 Pro (touch) | Graded |
|---|---|---|---|---|---|
| 1 | Flashcard, Spanish → English | recognition | reveal, then four grade keys | tap to reveal, tap a grade | self |
| 2 | Flashcard, English → Spanish | production | same | same, or type the answer | self or auto |
| 3 | Multiple choice, four options | recognition or production | one front key per option | tap the option | auto |
| 4 | Cloze in a sentence, four options | word in context, verb forms, `ser`/`estar`, `por`/`para` | one front key per option | tap, or type | auto |
| 5 | Gender drill (`el` / `la`) | noun gender | two front keys | tap | auto |
| 6 | Conjugation drill | verb form for person and tense | four options | tap, or type | auto |
| 7 | Word order (tiles) | syntax | move focus with Left/Right, Confirm places a tile | tap tiles | auto |

Auto-grading maps to scheduler grades: wrong → Again, right after a hint or a
second try → Hard, right → Good. Self-graded cards offer all four grades with
the next interval shown above each.

Distractors for exercises 3, 4 and 6 are chosen on the device with a
deterministic generator seeded by item and repetition count: same part of
speech, similar level, different meaning, same topic where possible. Verb cloze
distractors are other forms of the same verb. Authored "confusable" sets
override the generator for pairs such as `ser`/`estar`.

Match-pairs and typed dictation are left out of v1: the first is awkward on
keys, the second needs audio.

### 4.4 Input mapping

| Meaning | Key devices (X3, X4, X4 Classic) | X4 Pro |
|---|---|---|
| Answer 1–4, or grade Again/Hard/Good/Easy | the four front keys — the footer cell above each key shows what it does | tap the option or footer cell |
| Reveal / next | Confirm, or side Down | tap the card, or the lower side key |
| Previous / undo last grade | side Up | upper side key, or swipe right |
| Pause sheet (Resume, End session, Home, Settings, Light, Leave) | hold Back 0.6 s | tap the Home pad |
| Tinta's Home | pause sheet → Home | hold the Home pad |
| Back to lila | Back on Tinta's Home, or Leave | the same |
| Sleep | Power (lila) | Power (lila) |
| Light panel | — | lila's: swipe down from the top, or Light in the pause sheet or Settings |

On choice screens the four front keys are *answers*, not navigation. A thin
key-mapping layer in front of FreeInkUI turns a front-key press into
`ActionChoice(index)` when the active screen declares a choice bar; otherwise
keys go through FreeInkUI's normal focus routing (lists, settings, tiles).
Touch always goes through FreeInkUI interactions.

All three key devices have four front keys under the screen and two side keys,
so they share one interaction design. What differs is the physical
left-to-right order and spacing of the front keys. `ui/KeyMap` holds a
per-device table of physical key order and footer cell geometry. Tinta labels
the keys by position, so lila's button remapping does not apply inside it.

### 4.5 Screens

```
Home                                   Flashcard, answer shown
┌────────────────────────────────┐     ┌────────────────────────────────┐
│ Tinta            ▮▮▮▯  14:05   │     │ Repaso 12/40          ▮▮▮▯     │
│                                │     │▬▬▬▬▬▬▬▬▬▬▬▭▭▭▭▭▭▭▭▭▭▭▭▭▭▭▭▭▭▭▭▭│
│  Hoy                           │     │ SUSTANTIVO · f · informal · MX │
│  23 repasos · 10 nuevas        │     │                                │
│  ┌──────────────────────────┐  │     │   la chamba                    │
│  │  Empezar                 │  │     │   CHAHM-bah                    │
│  └──────────────────────────┘  │     │ ────────────────────────────── │
│  Racha: 12 días                │     │   job, work                    │
│                                │     │                                │
│  Lección 3.2  En la taquería   │     │   Ando buscando chamba.        │
│  Leer                          │     │   I'm looking for work.        │
│  Frases                        │     │                                │
│  Diccionario                   │     │   Standard: el trabajo.        │
│  Progreso                      │     ├───────┬───────┬───────┬────────┤
│  Ajustes                       │     │Otra   │Difícil│ Bien  │ Fácil  │
│                                │     │<10 min│  1 d  │  3 d  │  7 d   │
└────────────────────────────────┘     └───────┴───────┴───────┴────────┘

Cloze, four options                    Reader with gloss
┌────────────────────────────────┐     ┌────────────────────────────────┐
│ Lección 3.2  4/12              │     │ En el mercado            2/5   │
│                                │     │                                │
│  ¿Me ____ dos tacos al pastor, │     │ —Buenas tardes, ¿a cómo está   │
│  por favor?                    │     │ el jitomate?                   │
│                                │     │ —A veinte el kilo, güerita.    │
│  Could you bring me two tacos  │     │ —¿Y el [aguacate]?             │
│  al pastor, please?            │     │  ┌──────────────────────────┐  │
│                                │     │  │ el aguacate  avocado     │  │
│  1  trae        2  traes       │     │  │ ah-gwah-KAH-teh    ☆ add │  │
│  3  traigo      4  traen       │     │  └──────────────────────────┘  │
├───────┬───────┬───────┬────────┤     ├────────────────────────────────┤
│   1   │   2   │   3   │   4    │     │ ◀ palabra ▶   traducir   salir │
└───────┴───────┴───────┴────────┘     └────────────────────────────────┘
```

Screen list: Home, Session (one controller, seven exercise views), Lesson intro
(note pages), Lesson summary, Course map, Reader, Phrasebook, Dictionary
(search, entry, verb table), Progress (streak calendar, forecast, totals),
Settings, Pause sheet, "What day is it?" (without a trusted clock), First-run,
Storage error. The light panel is lila's; the sleep card is drawn for lila's
sleep screen (section 4.7).

### 4.6 E-paper refresh policy

- Answering an item costs two refreshes: one to show the result with the
  correct answer, one to show the next item. There is no separate
  "Correct!" screen.
- Tinta draws on lila's render task under its render lock; input is handled
  on the loop task, and a press that arrives during a refresh waits for the
  next frame rather than being lost.
- A new screen gets the board's screen refresh: a half refresh on the X3, fast
  on the X4 family, where a half refresh flashes. Every Nth transition is full
  (`FreeInkApp::setTransitionFullEvery`, default 8, a Tinta setting), and so
  are going Home and the first frame after Tinta opens.
- Render only when input arrived or state changed, not on every loop pass.
- The reader's word cursor on key devices moves often. It is presented as a
  window of the panel where the controller supports it (the X4's SSD1677),
  else as a fast full-frame refresh.

### 4.7 Sleep screen

E-paper holds an image with no power, so the sleep screen teaches. With lila's
sleep screen on Current Page, sleeping from inside Tinta leaves a card with one
of the learner's weakest words (Spanish, respelling, gloss, example) under the
date, with the streak and tomorrow's due count; lila adds its moon. Each sleep
moves on to another word. Before anything is learnt the card shows a word of
the current lesson. lila's other sleep screens apply unchanged.

The standalone firmware could also wake on a timer to show another word; lila
does not.

---

## 5. Typography

### 5.1 Source strikes

lila's built-in fonts, inherited from CrossPoint, are native X11 bitmap strikes,
tracked under `x11/`:

- `x11/font-adobe-100dpi-1.0.4/` — Adobe Times and Helvetica, 8, 10, 12, 14,
  18 and 24 pt at 100 dpi, in four styles each.
- `x11/font-crosspoint-100dpi/` — CrossPoint's hand-tuned 16 pt and 21 pt
  strikes that fill the gaps.

That gives eight pixel sizes per face and style: **11, 14, 17, 20, 22, 25, 29
and 34 px**. All are ISO10646-1 encoded and contain every character Spanish
needs (`á é í ó ú ü ñ ¿ ¡ « » ª º`) plus `– — ‘ ’ “ ” … €`.

Tinta converts the BDF strikes, not CrossPoint's generated `EpdFontData`
headers. Those headers are 140–190 KB each because they carry up to about 920
glyphs plus Unifont fallback, are 2-bit and DEFLATE-compressed, and need CrossPoint's
`GfxRenderer`, `FontDecompressor` and HAL to draw. The BDF is 1-bit to begin
with, so a 1-bpp FreeInkUI `BitmapFont` is the same pixels with nothing lost.

Measured cost of the 202-glyph Spanish/English subset, 1-bpp with metrics:

| Strike | Size | Strike | Size |
|---|---|---|---|
| Helvetica 14 px | 3.3 KB | Times 25 px | 6.0 KB |
| Helvetica 17 px | 3.7 KB | Times 29 px | 7.5 KB |
| Helvetica 20 px | 4.4 KB | Times 34 px | 9.8 KB |
| Helvetica Bold 25 px | 6.8 KB | Times Bold 34 px | 10.2 KB |

All 64 strikes would fit in about 400 KB. The curated set below is about
145 KB, plus about 197 KB for the five outline-rasterised headword strikes.

### 5.2 Type scale and roles

On these panels (roughly 220–260 ppi — **verify**) the largest strike, 34 px,
is about the size of 10–11 pt print. That is fine for sentences and too small for
a flashcard headword. The headword sizes (50, 58, 68, 75 and 87 px) are
therefore rasterised at build time from an outline font, **TeX Gyre Termes
Bold**, with FreeType's monochrome renderer and the font's own hints. Termes
is a free Times clone (GUST Font License) with the same cap height and
x-height as Adobe Times Bold; it reads slightly heavier, and sits naturally
above the bitmap body text. Two alternatives were tried and rejected: hqx
scaling of the bitmap strikes (hq2x/hq3x/hq4x) keeps notches from the small
source bitmaps, and Scale2x is rougher still. At 34 px and below the
hand-tuned Adobe bitmaps beat any outline rasterisation, so they stay. The
look is confirmed by eye on hardware.

| Role | Face and strike | Used for |
|---|---|---|
| Spanish display | Termes Bold 87 / 75 / 68 / 58 / 50 px, then Adobe Times Bold 34 px; largest that fits | Flashcard headword |
| Spanish text | Times Roman 29 px (text size L), 25 (M), 22 (S) | Sentences, dialogues, reader |
| Spanish emphasis | Times Bold, same size | Target word in a sentence |
| Spanish aside | Times Italic, same size | Speaker names, grammar examples |
| English gloss | Helvetica 25 px | Meaning on a card |
| English translation | Helvetica 20 px | Sentence translations, notes |
| Respelling | Helvetica Oblique 20 px | Pronunciation |
| Label | Helvetica Bold 14 px | `SUSTANTIVO · f`, tags |
| Chrome small / body / title | Helvetica 17, Helvetica 20, Helvetica Bold 25 px | Status bar, lists, headers (same sizes CrossPoint uses for its UI) |
| Keyboard keys | Helvetica 34 px | On-screen keyboard (X4 Pro) |

### 5.3 Conversion: `tools/bdf2freeink.py`

A small pure-Python BDF parser (BDF is plain text; no FreeType dependency) that
writes `freeink::ui::BitmapFont` data into `src/fonts/`.

- **Bit packing.** `DisplayTarget::drawGlyph` reads bit `gy * width + gx` from a
  continuous bit stream. BDF rows are byte-aligned, so the converter repacks
  each glyph without row padding.
- **Metrics.** `xAdvance` = `DWIDTH`; `xOffset` = BBX x offset; `yOffset` =
  `-(bbxY + bbxH)`.
- **Line metrics from the subset, not the BDF header.** Accented capitals
  (`Á É Í Ó Ú Ñ`) rise above `FONT_ASCENT`. The converter sets `ascent` to the
  tallest glyph in the subset so `¿Él?` does not collide with the line above.
- **Limits.** `FontGlyph.bitmapOffset` is 16-bit, so a strike's bitmap must stay
  under 64 KB. That caps the headword sizes: the 87 px strike's bitmap is
  about 60 KB, and nothing larger fits with the full charset. The converters
  fail the build if a strike overflows.
- **Outline strikes.** `tools/otf2freeink.py` rasterises the vendored Termes
  outline into the same `BitmapFont` data, with a pinned FreeType so hinting
  cannot drift between machines. `bdf2freeink.py` keeps `--scale` and
  `--smooth hqx|scale2x` for the comparison pages only.
- Generated data is committed, as lila does for its own fonts, so a firmware
  build needs no Python. `tools/gen-fonts.sh` regenerates `src/fonts/` from
  lila's `x11/` strikes after a glyph change there; `tools/check-fonts.sh`
  reports a stale strike.

### 5.4 Character set

`BitmapFont` covers one contiguous range `first..last`. Tinta's range is
**U+0020–U+00FF** (224 slots), which holds all Spanish letters. Typographic
punctuation lies outside it, so the converter places those glyphs in the unused
C1 slots at their Windows-1252 positions:

| Slot | Glyph | Slot | Glyph |
|---|---|---|---|
| U+0080 | € | U+0094 | ” |
| U+0091 | ‘ | U+0095 | • |
| U+0092 | ’ | U+0096 | – |
| U+0093 | “ | U+0097 | — |

The content compiler rewrites those characters to the slot codepoints when it
writes strings, so the device draws them with no runtime mapping. `…` needs no
slot; `DisplayTarget` draws U+2026 as three dots. One Python module,
`tools/charset.py`, defines the table for both tools, and a matching
`constexpr` table in `src/core/lang/Charset.h` serves the few strings built at
runtime. The compiler fails on any character outside the set.

Ticks, crosses, stars and arrows are not in the fonts. They are icons, drawn
with Pillow by `tools/gen-icons.py` into `src/icons/`.

### 5.5 Drawing mixed type through FreeInkUI

`DisplayTarget` is `final`, has eight font slots and draws text only through
`text(rect, string, style)`. Tinta needs about fourteen fonts and mixed styles
on one baseline. Both are handled without changing the SDK:

- **Chrome slots.** Slots 0–2 are small, body and title for FreeInkUI
  components. Slots 3–5 are body bold, keyboard keys and keyboard control
  labels.
- **Scratch slot.** Slot 7 belongs to Tinta's typesetter. Before each run it
  calls `setFont(7, font)` and then `text()`. Rendering is immediate-mode, so
  re-pointing a slot between calls is safe.
- **Baseline placement.** `DisplayTarget` draws a line at `rect.y + font.ascent`.
  The typesetter passes a rect of `y = baseline - ascent`,
  `height = yAdvance`, `width = measured + slack`, `maxLines = 1`, left
  aligned, so runs of different fonts share a baseline and nothing is
  ellipsised.
- **Measurement** reads `BitmapFont` advances directly. That keeps the
  typesetter freestanding and host-testable, and it agrees with the renderer
  because the charset guarantees every glyph is in range.

`core/text/Typesetter` takes spans (text, font role, decoration, token tag),
wraps them into a caller-provided fixed array of runs, and offers `draw()` and
`hitTest(x, y) → token`. Tap-to-gloss registers one FreeInkUI interaction for
the whole text block and resolves the word with `hitTest`.

Section 12 lists two small SDK changes that would remove the C1 remap and the
scratch slot. Neither is required.

### 5.6 Licences

- Adobe/DEC strikes: permissive notice in `COPYING`; reproduce it in `NOTICE`.
  "Times" and "Helvetica" are trademarks, so the About screen carries the
  notice from the BDF headers.
- CrossPoint 16 pt and 21 pt strikes: grid-fitted from URW++ Nimbus outlines.
  CrossPoint's README says to check URW's terms before redistributing.
  **Open item** before any public release; the fallback is to ship only the
  Adobe sizes (11, 14, 17, 20, 25, 34 px).

---

## 6. System architecture

### 6.1 Layers

```
┌──────────────────────────────────────────────────────────────────────┐
│ ui/        screens, theme, key map, typeset views       (FreeInkUI)  │
├──────────────────────────────────────────────────────────────────────┤
│ app/       App: navigation, session controller, settings, refresh    │
├──────────────────────────────────────────────────────────────────────┤
│ core/      pack reader · scheduler · queue · exercises · answer      │
│            check · typesetter · stats · usage log  (freestanding     │
│            C++17, no Arduino, host-tested)                           │
├──────────────────────────────────────────────────────────────────────┤
│ platform/  Board · Clock · StateFiles · PackFile · Power · Log       │
│            (headers in lib/Tinta/src/platform, implemented over      │
│            lila's HAL in src/activities/tinta/platform)              │
├──────────────────────────────────────────────────────────────────────┤
│ lila: TintaActivity · ActivityManager · GfxRenderer · HalDisplay ·   │
│ HalGPIO · HalStorage · HalClock · HalPowerManager                    │
└──────────────────────────────────────────────────────────────────────┘
```

`core/` reaches the outside only through three small interfaces — `PackSource`
(bytes), `StateStore` (files) and `Clock` (time) — so it runs unchanged in host
tests and on the device.

### 6.2 Repository layout

```
lib/Tinta/
  src/
    app/                    App, Screens, SessionController, View
    ui/                     Theme, Fonts, KeyMap, Strings, screens/*, views/*
    core/                   pack, srs, session, lang, text, stats, library,
                            reader, search, profile, usage
    platform/               the device interfaces (implemented in lila)
    fonts/  icons/          generated, committed
  content/                  course sources (YAML/TSV), ids.lock
  content-plan/             the plan for Units 2–12, check_plan.py
  assets/fonts/outline/     TeX Gyre Termes, for the headword strikes
  tools/                    packc, bdf2freeink, otf2freeink, gen-fonts.sh,
                            gen-icons.py, usage-report.py, progress-dump.py
  docs/                     pack-format.md, content-style.md, app-shell.md,
                            usage-log.md
src/activities/tinta/       TintaActivity and the platform layer
test/tinta/                 host tests for core/ and their fixtures
```

### 6.3 Build

`-DLILA_TINTA=1` in lila's `platformio.ini` builds Tinta into the C3, X4
Classic and X4 Pro environments and their release variants. The other boards
leave `lib/Tinta` out. Device differences come from lila's HAL at runtime
(touch, frontlight, RTC, screen size), not from `#ifdef`s in Tinta.

### 6.4 Opening and closing

`TintaActivity::onEnter()` allocates the `App`, which opens the course pack
and the learner's files, loads the profile, and restores the screens and the
review session saved when Tinta last closed (or asks for the date first,
section 6.8). `onExit()`, which lila runs before it sleeps or opens anything
else, writes everything held in RAM and frees it all. Sleeping from inside
Tinta first draws the sleep card (section 4.7).

### 6.5 Main loop

```
loop task (TintaActivity::loop):
  poll keys, taps, swipes and the Home pad into Tinta's input queue
  under the render lock, if the panel is not busy:
    route input: a choice bar takes front keys as ActionChoice(i),
                 otherwise FreeInkUI focus routing or interactions
    handle the action: update state, write journal and item state to SD
  ask for a frame if something changed
render task (TintaActivity::render):
  draw into the framebuffer and present with the strongest pending hint
```

SD writes finish before the next present; both go through lila's mutexes.

### 6.6 Memory budget (ESP32-C3: X3 and X4)

Everything below exists only while Tinta is open; a closed Tinta keeps about
220 bytes of static RAM.

| Use | Size |
|---|---|
| Item index: 16-bit state slot per pack item (2 bytes × 7,223 items) | 14 KB |
| Pack block cache (8 × 1 KB) and per-pass string arena (6 KB) | 14 KB |
| Usage log buffer | 1 KB |
| Screens, shared text buffers, card and exercise views | the rest |
| **Total** | **about 60 KB**; the `TNT` log lines give the heap before and after opening |

lila's framebuffer and render task are shared, not Tinta's. Rules carried over
from lila: no `String` or `std::string` in hot paths, no allocation in render
functions, locals under 256 bytes, constant tables `constexpr`,
`new (std::nothrow)` only.

### 6.7 Flash

Fonts and icons are about 0.35 MB of flash. The course pack costs none: it is
read from the SD card, block by block, through `core/pack/PackSource`.

### 6.8 Power and time

**Sleep and power** are lila's. Before lila sleeps or opens something else,
`TintaActivity::onExit()` saves the session and the profile.

**Time.** The scheduler works in whole days. `platform/Clock` answers two
questions: `today()` and `hasTimeOfDay()`.

| Clock | Behaviour |
|---|---|
| lila's RTC reads a plausible time (X3, X4 Classic, X4 Pro) | Local time is lila's: the RTC keeps UTC and lila's time zone (Settings › Clock, with daylight saving) gives the wall time. The study day rolls over at Tinta's "Day starts at" hour (04:00 by default). |
| No RTC (X4), or an RTC that reads earlier than the firmware date or the last day the device saw | At the first opening after power-on Tinta asks "What day is it?" with a choice bar: **Same day**, **Next day**, **Other date**. One press in the usual cases. The date holds until the next power-on; no time of day. |

- lila sets its clock over Wi-Fi (Settings › Clock). An RTC that is not trusted
  is used again once it is: at the next power-on after a sync.
- The confirmed date is stored in `profile.bin`. Picking "Next day" after
  several days away under-counts the gap; the scheduler tolerates that (items
  are simply treated as reviewed on time), but the streak will be wrong unless
  the learner picks the real date.

**Battery.** Tinta's status bar shows the percentage from lila's battery
reading.

---

## 7. Content system

### 7.1 Sources

Human-editable files under `content/`, reviewed like code:

```
content/
  units/03-taqueria/
    unit.yaml              title, goals
    lesson-2.yaml          new lemmas, grammar note, dialogue, sentences
  lexicon/core.tsv         lemma, pos, gender, gloss, level, register, mx flag, note
  lexicon/frequency.tsv    high-frequency lemmas outside the lessons
  verbs/irregular.yaml     overrides for the Python conjugator
  phrasebook/*.yaml
  stories/*.yaml
  confusables.yaml         authored distractor sets
  ids.lock                 stable item ids (generated, committed)
```

A sentence is written once, with its translation and the target word marked:

```yaml
- es: "¿Me {trae|traer:pres.3s} dos tacos al pastor, por favor?"
  en: "Could you bring me two tacos al pastor, please?"
  note: "Polite request with usted."
```

### 7.2 Compiler: `tools/packc`

Python. It reads the sources and writes `build/course.pack`.

1. **Tokenise** every Spanish sentence and link each token to a lemma.
   Unlinked content words are errors, so every word in the reader can be
   glossed without stemming on the device.
2. **Conjugate** every verb for five persons across the tenses in scope and
   store the forms. The device never conjugates.
3. **Respell** every lemma and phrase with Mexican rules (seseo, yeísmo, silent
   `h`, stress from accent rules), with an override list for `x` words and
   loans.
4. **Generate items** from lemmas and sentences, assigning ids from `ids.lock`.
5. **Validate** (errors fail the build):
   - every character is in the font charset;
   - every lemma has a gloss, a level and at least one example;
   - headwords fit the card width at some display strike; sentences fit a page;
   - each multiple-choice item has at least three valid distractors;
   - Peninsular lint: `vosotros`, `os`, `vuestro`, `-áis`/`-éis` endings, and a
     word list (`ordenador`, `móvil`, `zumo`, `patata`, `piso`, `vale`, `gafas`,
     `aparcar`, `coger`);
   - register tags present for every informal or vulgar lemma;
   - ids in `ids.lock` never change meaning or disappear.
6. **Emit** the pack, a size report and a human-readable dump for review.

The conjugator's tests compare it with Fred Jehle's verb database (CC BY-NC-SA,
fetched for tests only and never shipped: `tools/packc/tests/fetch_reference.py`),
and with a committed golden table.

### 7.3 Pack format

Little-endian, sections 4-byte aligned. Strings are NUL-terminated UTF-8 in the
font charset, deduplicated in one heap, referenced by 32-bit offset. The device
reads the pack from the SD card through a block cache; a string is copied into
a per-pass arena and stays valid until the next event or frame
(`core/pack/PackSource.h`, `Pack::beginPass()`). `docs/pack-format.md` has the
layouts.

```
Header   magic "TNTA", formatVersion, contentVersion, buildTime, size, crc32, locale "es-MX"
Directory  { tag, offset, size, count } per section
```

| Section | Contents | Access |
|---|---|---|
| `STRS` | string heap | by offset |
| `LEMM` | lemma records: Spanish, gloss, respelling, note, part of speech, gender, level, register flags, frequency rank, lesson, first example, verb-table index | by lemma id |
| `LKEY` | accent-folded Spanish keys → lemma id, sorted | binary search, prefix scan |
| `EKEY` | English keywords → lemma ids, sorted | binary search |
| `FORM` | accent-folded inflected forms → lemma id and form tag, sorted | binary search |
| `VERB` | conjugated forms, five persons × tenses | by verb-table index |
| `SENT` | sentence records: Spanish, English, token span, level, lesson | by sentence id |
| `TOKS` | tokens per sentence: byte start, length, lemma id, form tag | by span |
| `ITEM` | schedulable items: stable id, kind, lesson, two operands | by item index |
| `IUID` | sorted stable ids → item index | binary search |
| `LESS` | units and lessons: titles, item range, note range, dialogue | by lesson id |
| `NOTE` | grammar and culture notes as pre-split styled spans | by offset |
| `STOR` | dialogues and readings: title, level, lines (speaker, sentence id) | by story id |
| `PHRS` | phrasebook categories → sentence ids | by category |
| `CONF` | authored distractor sets | by lemma id |

The full course compiles to about 2.8 MB.

### 7.4 Course scope for v1

| Unit | Theme | Grammar focus |
|---|---|---|
| 0 | Sonidos y letras | Vowels, stress and written accents, `ñ`, `ll`/`y`, `j`/`g`, `h`, `r`/`rr`, seseo, `x` |
| 1 | Saludos y cortesía | Subject pronouns without `vosotros`, `tú` and `usted`, `ser` |
| 2 | ¿Quién eres? | Gender and articles, plurals, numbers to 30, `estar` |
| 3 | En la taquería | `querer`, `gustar`, polite requests, numbers to 100, prices |
| 4 | La familia y la casa | `tener`, possessives, adjective agreement, `hay` |
| 5 | La ciudad y el transporte | `ir a`, prepositions of place, directions |
| 6 | El día a día | Regular present, stem changes, reflexives, time |
| 7 | El mercado y el tianguis | Demonstratives, object pronouns, comparatives |
| 8 | Planes y clima | `ir a` + infinitive, weather, progressive |
| 9 | Ayer | Preterite, key irregulars |
| 10 | Antes | Imperfect; preterite versus imperfect |
| 11 | Salud y emergencias | `doler`, `tener que`, formal commands |
| 12 | Así se habla en México | Colloquial register, diminutives, Nahuatl loans |

Targets: about 44 lessons; about 600 lesson lemmas plus about 900 from a
frequency list; about 2,500 sentences; 44 dialogues and 20 short readings;
about 300 phrasebook entries; about 5,000 dictionary headwords.

### 7.5 Where the content comes from

Content is the largest part of the project and runs in parallel with the
firmware from M3 onward.

- **Course material** (lessons, dialogues, sentences, notes) is written for
  Tinta: drafted with an LLM against `docs/content-style.md`, then reviewed by
  the project's content reviewer (the owner's wife). Nothing ships unreviewed.
- **Review workflow.** The reviewer should not need the repo or a device.
  `packc --review` writes one printable page per lesson (HTML or PDF): every
  Spanish sentence with its translation, respelling, notes and register tags,
  with space for corrections. Each lesson file records `reviewed: {by, date,
  hash}`. Release builds refuse a lesson whose content hash differs from its
  reviewed hash, so an edit after review cannot ship unnoticed. Development
  builds only warn.
- **Frequency ranking** from an open subtitle-based frequency list.
- **Dictionary glosses** beyond the course from Wiktionary-derived data, which
  also carries "Mexico" regional labels.
- **Licences.** Wiktionary data is CC BY-SA, which would make the pack
  CC BY-SA. Confirm each source's licence before importing and record
  attributions in `NOTICE`. Firmware code is MIT, matching the SDK.

### 7.6 Stable ids

Progress is keyed by item `uid`, not by position. `ids.lock` maps a content key
(for example `vocab:chamba:recognise`) to a uid and is only ever appended to.
Editing a gloss or fixing a sentence keeps the uid; removing an item retires
it. A content update therefore never orphans or mis-assigns progress.

---

## 8. Learning engine

### 8.1 Scheduler

FSRS, ported from the open-spaced-repetition reference implementation and
pinned to one upstream version. Default weights are copied from that version,
not retyped. The host test suite checks the port against vectors generated by
the Python reference.

- Per-item state: stability, difficulty, due day, last review day, repetitions,
  lapses, phase (new, learning, review, relearning).
- A review is a handful of `float` operations. The C3 has no FPU; soft-float
  cost per review is negligible.
- Settings: desired retention (default 0.90), new items per day (10), review
  cap (100), maximum interval (365 days).
- **Learning steps are session-relative, not clock-relative.** A failed or new
  item returns after at least 3 other items, then after at least 8, then
  graduates to day-based scheduling. This suits short sessions on a device
  that sleeps between them and avoids minute-level timekeeping.
- Leeches: an item with 6 lapses is flagged, shown with its note, and offered
  for suspension.

### 8.2 Building a session

1. Scan the state file for items due today (sequential read, tens of
   milliseconds), sort by retrievability, apply the review cap.
2. Add new items from the current lesson in authored order, then from the
   frequency list, up to the daily limit.
3. Interleave kinds so the same lemma does not appear twice in a row.
4. For each item pick a format valid for its kind, its maturity and the device
   (no typed formats on key devices in v1).

### 8.3 Progress store

On the SD card under `/tinta/`:

| File | Contents | Write pattern |
|---|---|---|
| `items.bin` | header, then 16-byte state records for items that have been seen, in first-seen order | one 16-byte in-place write per review |
| `reviews.log` | 12 bytes per review: uid, time, grade, format, response time | append |
| `days.bin` | per-day totals: reviews, correct, new, seconds | one write per session |
| `profile.bin` | settings, streak, current lesson, the last confirmed date | write temp file, then rename |
| `session.bin` | the screens and the in-progress session, to resume | after each grade, and when Tinta closes |

```cpp
struct ItemState {        // 16 bytes
  uint32_t uid;
  uint16_t dueDay, lastDay;         // days since 2024-01-01
  uint16_t stability;               // days × 16, capped
  uint8_t  difficulty;              // scaled 1..10
  uint8_t  phase;                   // phase and learning step
  uint8_t  reps, lapses;
  uint16_t flags;                   // suspended, leech, starred
};
```

- In RAM: one 16-bit slot number per pack item (`0xFFFF` = unseen).
- **Order of writes per review:** append to `reviews.log`, then update
  `items.bin`, then repaint. If power fails between the two, boot detects the
  mismatch (journal count in the header) and replays the journal tail. If
  `items.bin` is missing or corrupt, it is rebuilt from the journal.
- The journal also lets a PC tool recompute FSRS weights for the learner later.
- **No SD card:** Tinta runs as a guest — lessons, reader, phrasebook and
  dictionary work, nothing is saved, and a banner says so.

### 8.4 Checking answers

For tiles and typed answers: lower-case, trim, collapse spaces, ignore `¿ ¡`
and final punctuation, then compare with the item's accepted answers.

- Exact match → Good.
- Differs only in accents → accepted as Hard, and the accent is shown
  (`esta` → `está`).
- Edit distance 1 on a word of six or more letters → "typo", Hard.
- A correct non-Mexican synonym → accepted, with the Mexican word shown.

### 8.5 Dictionary and reader

- **Lookup** is binary search over `LKEY`, `FORM` and `EKEY` in the pack. On the
  X4 Pro the FreeInkUI `qwertyKeyboard` with the `SpanishEs` layout filters as
  you type. On key devices the dictionary is a virtualised alphabetical list
  with a letter-jump menu.
- **Entry page:** headword, part of speech and gender, respelling, glosses,
  examples, verb table (one tense per page, five rows), and "add to my deck".
- **Vulgar entries are never hidden from the dictionary.** The "show vulgar
  words" setting keeps vulgar items out of exercises, distractors and the
  reader's word lists. In the dictionary a vulgar entry is always findable,
  because the warning is the point (`coger`: vulgar in Mexico; say `tomar` or
  `agarrar`). With the setting off, the entry shows its headword, a "vulgar"
  label and its note, and leaves the gloss out.
- **Peninsular words have entries too** (`ordenador`, `móvil`, `zumo`,
  `chaqueta`), each with a note giving the Mexican word, so a learner coming
  from a textbook finds what to say instead.
- **Reader:** stories are sentence lists; the typesetter paginates them for the
  device and text size and keeps page starts in a small array. On key devices,
  Left/Right move a word cursor and side Up/Down turn pages; Confirm opens the
  gloss. On the X4 Pro, tap a word. A footer action shows the English
  translation of the current sentence. Readings end with comprehension
  questions.

### 8.6 Usage log

The owner wants to learn from real use: which lessons, words and exercise
formats work, and where the interface gets in the way. Tinta therefore keeps a
second, richer record beside the review journal: the usage log,
`/tinta/usage-0001.log` and up on the SD card (numbered files, because the
state store has no rename; layout in `docs/usage-log.md`). It never leaves the
card except when the owner copies it.

- **It is not learner state.** Scheduling never reads it, losing it loses no
  progress, and the review journal is always written first. A failed usage
  write is counted and otherwise ignored. Guest mode (no card) records nothing.
- **Format:** append-only binary records with a one-byte type, the wall-clock
  time when there is one and uptime otherwise, and a boot record (each opening
  of Tinta) that names the firmware version, device and pack edition, so a log
  can be joined to the right `ids.lock`. Records are buffered in RAM and
  written with the SD write the app is already making (an answer, closing), so
  recording adds no refresh and at most one append per answered item. Each
  append is one checksummed chunk, so a torn write is skipped on decode. A new
  file starts at 1 MiB and 16 files are kept: about 7.5 KB for a 20-minute day,
  so years of use.
- **What is recorded:**
  - *Learning:* each item shown (uid, format, the options offered), each answer
    (option chosen or text typed, right or wrong, grade, response time,
    attempts), undo, session start and end (kind, planned and done counts, how
    it ended), lesson pages and time on each, dialogue English toggles.
  - *Reading and lookup:* story opened and finished, page turns, each word
    glossed, sentence translations shown, words starred, quiz answers;
    dictionary searches (the text typed, and whether anything matched), entries
    opened, verb tables viewed; phrasebook categories opened and practised.
  - *Interface:* every screen entered (so dwell time and paths follow), keys
    and taps that did nothing, presses that arrived during a refresh, settings
    changed.
  - *Device:* openings, confirmed dates and day rollovers, storage and pack
    errors.
- It is always on; `UsageLog::setEnabled()` is ready for a setting.
- **`tools/usage-report.py`** on the PC decodes a log to CSV and writes a
  report: accuracy and time by lesson, word and format; the wrong options most
  often chosen; words glossed or searched most (candidates for new lessons) and
  searches that found nothing (candidates for new headwords); where sessions
  and lessons are abandoned; screen paths, dwell and dead presses.

---

## 9. Testing

| Layer | What | How |
|---|---|---|
| `core/` | FSRS against reference vectors; pack reader (in memory and through the SD block cache); queue building; distractor determinism; answer checking; typesetter wrapping and hit-testing; progress store under power cuts; usage log encoding | Host tests in `test/tinta`, `sh test/tinta/run.sh` and `ctest` |
| Content | Schema, charset, lint, id stability, conjugator | `packc --check` and the compiler's tests, in CI (`tinta-course`) |
| Tools | Font conversion, the usage report against a log the engine wrote | `python3 -m unittest discover -s tests` in `tools/` |
| Fonts | Generated data equals what `x11/` gives | `tools/check-fonts.sh` |
| Hardware | Refresh quality and ghosting, key and touch mapping, the sleep card, SD removal mid-session | By hand on each device |

---

## 10. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| Content quality or wrong dialect | Teaches errors | Review gate enforced by the compiler (section 7.5); Peninsular lint; register tags |
| One reviewer for 44 lessons | Review becomes the bottleneck | Printable per-lesson review pages (`packc --review`), reviewed in unit-sized batches |
| URW-derived strikes cannot be redistributed | Lose the 22 and 29 px sizes | The Adobe-only scale is already defined (section 5.6) |
| Fast-refresh ghosting from frequent card flips | Smeared text | Full refresh every N transitions, a Tinta setting |
| Front key order and position versus footer cells | Labels do not line up with keys | Per-device key order and footer geometry in `ui/KeyMap` |
| X4 has no clock | Wrong dates, broken streak | One-press date prompt at power-on; the scheduler works in whole days |
| SD card missing, removed or corrupt | Progress lost | Journal-first writes, rebuild from the journal, guest mode, storage error screen |
| A new pack edition drops content | Orphaned progress | Stable ids (section 7.6); a saved session whose lesson is gone is dropped |
| No audio | Pronunciation limited | Respelling, Unit 0, stated limitation |

---

## 11. Status

| Milestone | State |
|---|---|
| M1 fonts and typography | done |
| M2 app shell | done; inside lila, sleep, power, the clock's time zone and the light are lila's |
| M3 content pipeline and pack | done; the pack is read from the SD card |
| M4 scheduler and flashcards | done |
| M5 exercises and lessons | done |
| M6 reader, phrasebook, deck, search, sleep card | done |
| M7 full course | content drafted in full: 44 lessons, 23 readings, 307 phrases, about 1,000 deck words and 3,550 more dictionary headwords. Open: the reviewer's pass over every lesson, after which `packc --release` passes and the release attaches a reviewed pack |

Open on hardware: ghosting with the X4 family's fast screen refresh, footer
cells against the keys on each device, and the sleep card.

**After v1 (M8):**

- BLE keyboard for typed answers on key devices.
- SD expansion packs; Anki import tool on the PC.
- B1 content (subjunctive, conditional, future).
- PC tool to refit FSRS weights from `reviews.log`.
- Match-pairs exercise on touch.
- A Record usage setting.

---

## 12. Optional SDK changes

None is required. Each removes a workaround in Tinta's text drawing.

| Change | Removes |
|---|---|
| Sparse codepoint ranges in `BitmapFont` | The C1 slot remap (section 5.4) |
| Configurable `DisplayTarget::FONT_SLOTS`, or a public "draw run at baseline with this font" call | The scratch-slot technique (section 5.5) |

---

## 13. Decisions and open questions

Decided:

- **Name:** Tinta. In lila's Home Menu it is **Learn Spanish**.
- **Content review:** the owner's wife reviews all course content.
- **Headword strikes:** rasterised from TeX Gyre Termes Bold (50–87 px), after
  trying Scale2x and hqx scaling of the bitmap strikes. Adobe bitmaps stay for
  34 px and all body text.
- **Level:** A1–A2 in v1, B1 after.
- **Grades:** four (Again, Hard, Good, Easy).
- **File formats** on the card (`profile.bin`, `session.bin`, the usage log)
  are the standalone firmware's; ids and fields it used stay reserved.

Open:

1. **Content licence.** CC BY-SA if Wiktionary data is used.
2. **lila's 22 and 29 px strikes.** Resolve the URW terms before any public
   release (section 5.6).

# Tinta — a Mexican Spanish tutor for Xteink e-paper readers

Design and implementation plan for the Xteink X3, X4, X4 Classic and X4 Pro.
*Tinta* means "ink".

Status: plan only — nothing is built yet. Written 2026-10-03 against the local
checkouts of `../freeink-sdk` (HEAD `0a6da12`) and `../crosspoint-reader`
(HEAD `ab83debc`). Facts taken from those repos are listed in
[Appendix A](#appendix-a--facts-this-plan-relies-on); anything not yet proven on
hardware is marked **verify**.

---

## 1. Summary

Tinta is a standalone, offline firmware that turns an Xteink e-paper reader into
a pocket Spanish tutor for English speakers, teaching the Spanish spoken in
Mexico: Mexican vocabulary, `ustedes` instead of `vosotros`, Mexican
pronunciation and everyday register.

It is a new firmware built on the FreeInk SDK. It is not a fork of CrossPoint;
it reuses CrossPoint's bitmap Times and Helvetica strikes.

| Decision | Choice | Why |
|---|---|---|
| Targets | Four devices, three builds: `x3x4` (ESP32-C3, X3 and X4 in one binary), `x4c` and `x4pro` (ESP32-S3) | X3 and X4 share a pinout and are told apart at boot; the two S3 boards have different pinouts and the SDK has no detector for them |
| UI stack | FreeInkUI (`FreeInkApp` + `DisplayTarget`) | No external graphics library; runs in host tests and the simulator |
| Fonts | CrossPoint's X11 BDF strikes, converted to FreeInkUI `BitmapFont` headers | The BDF is 1-bit, so conversion is lossless; a Spanish/English subset costs 3–11 KB per strike |
| Typeface roles | **Times = Spanish, Helvetica = English and chrome** | The typeface tells the learner which language they are reading |
| Content | One memory-mapped binary pack embedded in the firmware image | Zero RAM, zero SD dependency for the course, strings passed to the UI without copying |
| Linguistics | Done at build time in Python; the device only reads tables | Conjugation, respelling and tokenising are testable off-device |
| Scheduling | FSRS spaced repetition, four grades | Four grades map one-to-one onto the four front keys of the X3, X4 and X4 Classic |
| Progress | SD card: fixed-size state file plus append-only review journal | Survives power loss; user can back it up |
| Radio | Off in v1 (no Wi-Fi, no BLE) | Saves RAM, flash and battery; nothing in v1 needs it |
| Audio | None | None of the four board profiles has audio; pronunciation is taught with respelling |

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

- A structured A1–A2 course: 13 units, about 44 lessons, about 1,500 lemmas.
- Daily spaced-repetition review in sessions of 5–10 minutes.
- Seven exercise types (section 4.3), all usable with keys only.
- Graded dialogues and short readings with word glossing.
- Phrasebook for travel situations in Mexico.
- Spanish–English dictionary of about 5,000 headwords with verb tables.
- Streak, statistics and a sleep screen that shows a word to learn.
- Development and regression testing in the FreeInk simulator, without hardware.

### Non-goals for v1

- Audio, speech or listening exercises (no hardware for it).
- Wi-Fi sync, accounts, OTA downloads.
- EPUB reading. Tinta is not a general reader; CrossPoint does that.
- European Spanish, voseo, or other regional variants.
- Landscape orientation.

### Later (section 11, milestone M8)

Typed answers with a BLE keyboard on key devices, SD expansion packs, Anki import,
B1 content.

---

## 3. Hardware targets

Four devices in two MCU families, all with 16 MB flash. "Key devices" in this
plan means the X3, X4 and X4 Classic; the X4 Pro is the only touch device.

| | X3 | X4 | X4 Classic | X4 Pro |
|---|---|---|---|---|
| MCU | ESP32-C3 | ESP32-C3 | ESP32-S3 | ESP32-S3 |
| RAM | about 380 KB, no PSRAM | about 380 KB, no PSRAM | internal plus 8 MB PSRAM | internal plus 8 MB PSRAM |
| Panel | 792×528 | 800×480 | 800×480 | 800×480 |
| Controller | UC8253 or UC8279d, per batch | SSD1677; variants probed at boot | SSD1677, UC8179 or UC8279, per unit | SSD1677, UC8179 or UC8279, per batch |
| Portrait logical size | 528×792 | 480×800 | 480×800 | 480×800 |
| Framebuffer | 52,272 bytes | 48,000 bytes | 48,000 bytes | 48,000 bytes |
| Input | 4 front keys, 2 side keys, Power | same as X3 | 4 bottom keys, 2 side keys, Power | GT911 touch, Home pad, 2 side keys, Power |
| Key wiring | ADC ladder | ADC ladder | discrete GPIOs | discrete GPIOs |
| Frontlight | none | none | none | warm/cold PWM |
| RTC | DS3231 | **none** | BM8563 | BM8563 |
| Battery | BQ27220 gauge | ADC | CW2017 gauge | CW2017 gauge |
| SD | SPI, shares the display bus | SPI, shares the display bus | 1-bit SDMMC | 1-bit SDMMC |
| USB mass storage | no | no | possible | possible |
| Build env | `x3x4` | `x3x4` | `x4c` | `x4pro` |

Consequences:

- **Three builds.** The X3 and X4 share a pinout, so one C3 binary carries both
  profiles and `selectXteinkDevice()` picks one at boot. This is the SDK's
  documented path and what CrossPoint ships. The X4 Classic and X4 Pro share
  the S3 and the glass but not the pinout (the Pro's frontlight and touch-power
  pins are front keys on the Classic), and the SDK has no detector for them, so
  each gets its own env, as in CrossPoint.
- **Design to the C3.** RAM budgets and refresh behaviour are set by the X3 and
  X4. The S3's PSRAM is not required by anything.
- **Two input models, two screen sizes.** The three key devices expose the same
  seven logical keys; the X4 Pro is touch-first with two keys. Layouts adapt
  to 528×792 and 480×800. Section 4.4 defines one semantic mapping.
- **Per-batch panel controllers** are resolved at boot by the SDK's
  `XteinkDetect` (a bus probe; a factory NVS value on the X4 Classic). Tinta
  uses only the `FreeInkDisplay` facade, never a driver directly.
- **On the C3 boards the SD card shares the display SPI bus.** All SD and
  display traffic runs on the loop task; no background SD I/O.
- **The X4 has no clock.** It has no RTC, and CrossPoint's sleep path powers
  the SoC off on battery, so no time survives sleep. Section 6.8 defines how
  scheduling works on it.
- **Waking is a cold boot on every device.** Session state is always restored
  from the SD card.

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
| Pause menu | hold Back 0.6 s | Home pad |
| Home screen | pause menu → Home | hold Home pad |
| Sleep | Power | Power |
| Light panel | — | swipe down from the top, or pause menu |

On choice screens the four front keys are *answers*, not navigation. A thin
key-mapping layer in front of FreeInkUI turns a front-key press into
`ActionChoice(index)` when the active screen declares a choice bar; otherwise
keys go through FreeInkUI's normal focus routing (lists, settings, tiles).
Touch always goes through FreeInkUI interactions.

All three key devices have four front keys under the screen and two side keys,
so they share one interaction design. What differs is the physical
left-to-right order and spacing of the front keys, and the wiring (an ADC
ladder on the X3 and X4, discrete GPIOs on the X4 Classic). `InputManager`
hides the wiring. `ui/KeyMap` holds a per-device table of physical key order
and footer cell geometry, confirmed on each device in M0.

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
Settings, Pause sheet, Light sheet (X4 Pro), Set clock, "What day is it?"
(X4), Sleep screen, First-run, Storage error.

### 4.6 E-paper refresh policy

- Answering an item costs two refreshes: one to show the result with the
  correct answer, one to show the next item. There is no separate
  "Correct!" screen.
- Use `FAST_REFRESH` within a session. Promote to `FULL_REFRESH` every Nth
  transition (`FreeInkApp::setTransitionFullEvery`, default 8, user setting)
  and at session start and end.
- Render only when input arrived or state changed, not on every loop pass.
- Use `presentAsync()` and `display.refreshBusy()` with
  `InputManager::beginAsync()`, so presses during a waveform are queued, not
  lost. Accumulate the strongest refresh hint between presents.
- The reader's word cursor on key devices moves often. Try `displayWindow()`
  for the cursor rectangle; fall back to a fast full-frame refresh if a
  controller variant does not support it. **Verify** on each controller.

### 4.7 Sleep screen

E-paper holds an image with no power, so the sleep screen teaches. It shows one
item from the learner's weakest words (Spanish, respelling, gloss, example),
the streak, and tomorrow's due count.

Optional rotation: a timer wake a few times a day redraws the screen with
another word and goes back to sleep, skipping night hours. This needs the SoC
to stay powered in sleep, so it is offered only on the X4 Classic and X4 Pro;
on the X3 and X4 the sleep path cuts battery power (section 6.8) and the word
chosen at sleep time stays until the next wake. Default is off until sleep
current is measured.

---

## 5. Typography

### 5.1 Source strikes

CrossPoint's built-in fonts are native X11 bitmap strikes, tracked in its repo
under `x11/`:

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
writes `freeink::ui::BitmapFont` headers into `src/fonts/`.

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
- Generated headers are committed, as CrossPoint does, so a firmware build
  needs no Python. CI regenerates them and fails on a diff.
- The BDF files are copied into `assets/fonts/bdf/` by
  `tools/sync-fonts.sh ../crosspoint-reader`, so builds do not depend on a
  sibling checkout. CrossPoint's 21 pt Times strikes currently have uncommitted
  edits; sync from a committed revision and record it.

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

Ticks, crosses, stars and arrows are not in the fonts. They are icons, generated
from the SDK's vendored Lucide set with `gen_icons.py`.

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
│ app/       navigation, session controller, settings, sleep policy    │
├──────────────────────────────────────────────────────────────────────┤
│ core/      pack reader · scheduler · queue · exercises · answer      │
│            check · typesetter · stats        (freestanding C++17,    │
│            no Arduino, host-tested)                                  │
├──────────────────────────────────────────────────────────────────────┤
│ platform/  Board · Clock · Storage · Light · Power   (thin adapters) │
├──────────────────────────────────────────────────────────────────────┤
│ FreeInk SDK: BoardConfig · FreeInkDisplay · InputManager ·           │
│ SDCardManager · XteinkDetect · PowerManager · Rtc · BatteryMonitor · │
│ FrontlightManager · FreeInkUI · Icons                                │
└──────────────────────────────────────────────────────────────────────┘
```

`core/` reaches the outside only through three small interfaces — `PackSource`
(bytes), `StateStore` (files) and `Clock` (time) — so it runs unchanged in host
tests, in the simulator and on the device.

### 6.2 Repository layout

```
spanish/
  platformio.ini            envs: x3x4, x4c, x4pro (+ release variants)
  partitions.csv            same table as CrossPoint
  freeink-sdk/              git submodule; platformio.local.ini may point at ../freeink-sdk
  src/
    main.cpp                setup(), loop()
    platform/               Board, Clock, Storage, Light, Power
    app/                    App, Navigator, SessionController, Settings, SleepPolicy
    ui/                     Theme, Fonts, KeyMap, screens/*, views/*
    core/
      pack/                 Pack, PackSource, tables, binary search
      srs/                  Fsrs, ItemState, DayQueue
      session/              SessionBuilder, ExercisePicker, Distractors
      lang/                 Utf8, Fold, Charset, AnswerCheck
      text/                 Typesetter
      stats/                Streak, DayLog
    fonts/                  generated BitmapFont headers (committed)
    icons/                  generated icons (committed)
  content/                  course sources (YAML/TSV), ids.lock
  assets/fonts/bdf/         vendored strikes
  tools/
    bdf2freeink.py  charset.py  sync-fonts.sh
    build_assets.py         PlatformIO pre-script: rebuilds the pack when content changes
    packc/                  content compiler and validators
    sim/                    build.sh, run.sh, golden screenshots
  test/host/                unit tests for core/, run.sh
  docs/                     pack-format.md, content-style.md, hardware-notes.md
```

### 6.3 Build environments

```ini
[base]
platform = <pioarduino platform-espressif32 55.03.311, the release CrossPoint builds on>
framework = arduino
board_build.flash_mode = dio
board_build.flash_size = 16MB
board_build.partitions = partitions.csv
build_unflags = -std=gnu++11 -fexceptions
build_flags =
  -std=gnu++2a -fno-exceptions
  -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1
  -DEINK_DISPLAY_SINGLE_BUFFER_MODE=1
extra_scripts = pre:tools/build_assets.py      ; builds the pack if content changed
lib_deps =
  BoardConfig=symlink://freeink-sdk/libs/hardware/BoardConfig
  EInkDisplay=symlink://freeink-sdk/libs/display/FreeInkDisplay
  InputManager=symlink://freeink-sdk/libs/hardware/InputManager
  BatteryMonitor=symlink://freeink-sdk/libs/hardware/BatteryMonitor
  SDCardManager=symlink://freeink-sdk/libs/hardware/SDCardManager
  XteinkDetect=symlink://freeink-sdk/libs/hardware/XteinkDetect
  PowerManager=symlink://freeink-sdk/libs/hardware/PowerManager
  Rtc=symlink://freeink-sdk/libs/hardware/Rtc
  FreeInkUI=symlink://freeink-sdk/libs/ui/FreeInkUI
  Icons=symlink://freeink-sdk/libs/assets/Icons

[env:x3x4]                 ; X3 and X4, one binary, selected at boot
extends = base
board = esp32-c3-devkitm-1
build_flags = ${base.build_flags} -DFREEINK_DEVICE_X3=1 -DFREEINK_DEVICE_X4=1

[s3]                       ; shared by the two ESP32-S3 boards
extends = base
board = esp32-s3-devkitc1-n16r8
board_build.mcu = esp32s3
board_build.arduino.memory_type = dio_opi
build_flags = ${base.build_flags} -DBOARD_HAS_PSRAM -DUSE_BLOCK_DEVICE_INTERFACE=1

[env:x4c]                  ; X4 Classic: keys only
extends = s3
build_flags = ${s3.build_flags} -DFREEINK_DEVICE_X4CLASSIC=1

[env:x4pro]                ; X4 Pro: touch and frontlight
extends = s3
build_flags = ${s3.build_flags} -DFREEINK_DEVICE_X4PRO=1
lib_deps = ${base.lib_deps}
  FrontlightManager=symlink://freeink-sdk/libs/hardware/FrontlightManager
```

Notes:

- Release files are named per device (`tinta-x3-x4.bin`, `tinta-x4-classic.bin`,
  `tinta-x4-pro.bin`). The two S3 images are not interchangeable: the Pro image
  would drive the Classic's key pins as frontlight outputs.
- Device differences are read from `BoardConfig::ACTIVE` at runtime (touch,
  frontlight, RTC, gauge, bezel insets), not from `#ifdef`s in app code. Only
  `platform/Board` knows which env it was built for.
- The platform is pinned to `55.03.311`, which CrossPoint builds on and which
  is already installed; the SDK sample pins the older `55.03.37`.
- The partition table is CrossPoint's (`app0`/`app1` of 0x640000 each), so a
  device already running CrossPoint can take Tinta as an app-slot update.
- The simulator's `build-firmware.sh` compiles only the display, drivers and
  `InputManager` from the SDK. `tools/sim/build.sh` wraps it and passes the
  extra SDK sources Tinta uses (`FreeInkUI.cpp`, `Rtc`, `BatteryMonitor`,
  `SDCardManager`, `PowerManager`, `FrontlightManager`). It builds four
  bundles: `X3`, `X4`, `X4CLASSIC`, `X4PRO`.

### 6.4 Boot sequence

```cpp
void setup() {
  BoardConfig::holdPowerRails();              // battery latch
  Serial.begin(115200);
#if FREEINK_MCU_C3
  if (freeink::selectXteinkDevice())          // X3 vs X4, then UC8253 vs UC8279d
    display.setDisplayX3();
#else
  freeink::applyXteinkDisplayController();    // X4 Pro: bus probe; X4 Classic: factory NVS value
#endif
  input.begin();                              // reject wakes without a held Power key
  storage.begin();                            // SD before display: shared bus on the C3 boards
  display.begin();
  clock.begin(); battery.begin(); light.begin();
  ui.begin(display);                          // DisplayTarget only after display.begin()
  app.begin();                                // open pack, load profile, resume or Home
}
```

The order follows the SDK's documented contract: device selection before
`SDCardManager::begin()` and `FreeInkDisplay::begin()`; `DisplayTarget`
constructed after `display.begin()`.

Target: Power press to a readable screen in under 2 s. Waking from deep sleep
is a chip reset, so the app restores the saved session and paints it directly.

### 6.5 Main loop

```
loop:
  collect input (async queue: key presses, taps, swipes, Home pad)
  if a choice bar is active and a front key was pressed → ActionChoice(i)
  else build InputSnapshot → app.render() or app.route()
  handle the action: update state, write journal and item state to SD
  if dirty: render into the framebuffer, remember the strongest hint
  if hint pending and !display.refreshBusy(): presentAsync(hint)
  idle timer → save session, draw sleep screen, deep sleep
```

SD writes finish before the next display transfer starts. Both happen on the
loop task, which is the bus-ownership rule for the X3 and X4.

### 6.6 Memory budget (ESP32-C3: X3 and X4)

| Use | Size |
|---|---|
| Framebuffer, single-buffer mode | 52 KB |
| Loop task stack (FreeInkUI's default) | 16 KB |
| Input polling task | 4 KB |
| Item index: 16-bit state slot per pack item (2 bytes × about 8,500 items with the deck) | ≤ 20 KB |
| Session queue, typesetter runs, UI interaction tables | ≤ 8 KB |
| SdFat and file buffers | ≤ 8 KB |
| Content pack | 0 (flash-mapped) |
| **Total** | **about 100 KB** of roughly 380 KB |

Rules carried over from CrossPoint's experience on this chip: no `String` or
`std::string` in hot paths, no allocation in render functions, locals under
256 bytes, constant tables `constexpr`, `new (std::nothrow)` only.

### 6.7 Flash budget

| Part | Budget |
|---|---|
| Arduino core, IDF, SDK libraries, app code | ≤ 1.4 MB |
| Fonts and icons | ≤ 0.45 MB |
| Content pack | ≤ 4.0 MB (the 44-lesson course is 0.77 MB; projected 3.3–3.8 MB with the frequency deck and a 5,000-headword dictionary) |
| **Image** | **≤ 5.85 MB** in a 6.5 MB app slot |

The pack is linked into the image as read-only data with an `.incbin` stub, so
it is memory-mapped by the flash MMU and costs no RAM. Host tests and the
simulator load the same file from disk through the same `PackSource`
interface.

### 6.8 Power and time

**Sleep.** Idle timeout (default 3 min) or Power → save session, draw sleep
screen, `display.deepSleep()`, `PowerManager::powerDownRailsForSleep()`, then
sleep with Power-key wake.

- On the S3 boards the power latch (GPIO1) is held through deep sleep, as
  CrossPoint does, so the SoC stays in deep sleep.
- On the C3 boards CrossPoint's sleep path drives GPIO13 low, which it
  describes as cutting battery power to the SoC. Tinta follows the same path.
  **Verify** on each board what stays powered.
- Either way, waking is a reset and the app restores from `session.bin`.
- The X4 Pro frontlight turns off on sleep and restores on wake if it was on.

**Time.** The scheduler works in whole days. `platform/Clock` answers two
questions: `today()` and `hasTimeOfDay()`.

| Device | Source | Behaviour |
|---|---|---|
| X3, X4 Classic, X4 Pro | hardware RTC | Date, time and UTC offset are set once (first run, or Settings). The day rolls over at 04:00 local. The status bar shows the time. |
| X4 | none | At each power-on Tinta asks "What day is it?" with a choice bar: **Same day**, **Next day**, **Other date**. One press in the usual cases. No clock in the status bar. |

- On RTC devices, if the clock reads earlier than the last recorded review or
  the firmware build date, Tinta asks for the date before scheduling anything.
- On the X4 the date the learner confirms is stored in `profile.bin`. Picking
  "Next day" after several days away under-counts the gap; the scheduler
  tolerates that (items are simply treated as reviewed on time), but the streak
  will be wrong unless the learner picks the real date.
- Two ways to give the X4 a real clock are left for later: keeping it in true
  deep sleep so the SoC's internal timer survives (depends on measured sleep
  current and timer drift), or NTP over Wi-Fi at wake (M8).

**Battery.** Percentage in the status bar from `BatteryMonitor`, which reads a
gauge or the ADC according to the active profile; a low-battery screen below
5 %.

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

The conjugator is checked in CI against an external reference verb table.
Choose the reference in M3 and check its licence; it is used for tests only.

### 7.3 Pack format

Little-endian, sections 4-byte aligned, readable in place. Strings are
NUL-terminated UTF-8 in the font charset, deduplicated in one heap, referenced
by 32-bit offset. Because strings are NUL-terminated and flash-mapped, a
`const char*` into the pack goes straight to FreeInkUI, which borrows strings
and never copies them.

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

Fixed-size records, sketched (final layouts go in `docs/pack-format.md` in M3):

```cpp
struct Lemma {            // 28 bytes
  uint32_t es, en, pron, note;      // string offsets (0 = none)
  uint32_t firstExample;            // sentence id
  uint16_t freqRank, lesson;
  uint8_t  pos, gender, level, flags;
  uint8_t  exampleCount, reserved;
  uint16_t verbTable;               // 0xFFFF = not a verb
};
struct Item {             // 16 bytes
  uint32_t uid;                     // stable across pack versions
  uint8_t  kind, flags;
  uint16_t lesson;
  uint32_t a, b;                    // lemma or sentence id; token index or form tag
};
```

Estimated size for v1: about 1.5–2 MB (sentences and tokens about 0.6 MB, verb
forms about 0.3 MB, dictionary about 0.5 MB, the rest indexes and notes).

A later version reads the same format from the SD card for expansion packs,
through a `PackSource` that pages blocks from a file.

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
| `profile.bin` | settings, streak, current lesson, UTC offset | write temp file, then rename |
| `session.bin` | the in-progress session, for resume after sleep | on sleep |

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

- **Lookup** is binary search over `LKEY`, `FORM` and `EKEY` in flash. On the
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
formats work, and where the interface gets in the way. The device therefore
keeps a second, richer record beside the review journal: the usage log,
`/tinta/usage-0001.log` and up on the SD card (numbered files, because the
state store has no rename; layout in `docs/usage-log.md`). It never leaves the
card except when the owner copies it; the firmware has no radio code in v1.

- **It is not learner state.** Scheduling never reads it, losing it loses no
  progress, and the review journal is always written first. A failed usage
  write is counted and otherwise ignored. Guest mode (no card) records nothing.
- **Format:** append-only binary records with a one-byte type, the wall-clock
  time when the device has one and uptime otherwise, and a boot record that
  names the firmware version, build, device and pack edition, so a log can be
  joined to the right `ids.lock`. Records are buffered in RAM and written with
  the SD write the app is already making (an answer, a sleep), so recording
  adds no refresh and at most one append per answered item. Each append is one
  checksummed chunk, so a torn write is skipped on decode. A new file starts at
  1 MiB and 16 files are kept: about 9 KB for a 20-minute day, so years of use.
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
    and taps that did nothing, presses that arrived during a refresh, time from
    press to finished frame, refresh counts by kind, settings changed.
  - *Device:* boot, wake and sleep with cause, battery level and charging
    state, clock changes, storage and pack errors.
- **A setting** (Settings > Study > Record usage, default on) turns it off.
- **`tools/usage-report.py`** on the PC decodes a log to CSV and writes a
  report: accuracy and time by lesson, word and format; the wrong options most
  often chosen; words glossed or searched most (candidates for new lessons) and
  searches that found nothing (candidates for new headwords); where sessions
  and lessons are abandoned; screen paths, dwell and dead presses; battery use
  per hour.

---

## 9. Testing

| Layer | What | How |
|---|---|---|
| `core/` | FSRS against reference vectors; pack reader against a sample pack; queue building; distractor determinism; answer checking; typesetter wrapping and hit-testing | Host unit tests, plain C++17, `test/host/run.sh` (same pattern as the SDK's host suites) |
| Content | Schema, charset, lint, id stability, conjugator against the reference table | `packc --check` in CI |
| Fonts | Regenerated headers equal committed ones; specimen screenshot | CI and simulator |
| Whole firmware | Scripted flows on all four devices: boot, lesson, review session, day rollover (and the X4's date prompt), sleep and resume, SD missing, low battery | FreeInk simulator: `freeink-sim press`, `tap`, `expect`, `capture --wait-refresh`, virtual clock, golden PNGs |
| Hardware | Refresh quality and ghosting, key and touch mapping, sleep current, RTC retention, SD removal mid-session, battery gauge | Checklist per milestone in `docs/hardware-notes.md` |

Simulator notes:

- Build bundles with `--device X3`, `X4`, `X4CLASSIC` and `X4PRO`; the
  simulator runs the real display drivers, the ADC key ladder, the X4 Classic's
  discrete keys and the X4 Pro's GT911. The SDK's own simulator suite already
  covers the X3, X4 Classic and X4 Pro shapes.
- Most flows are written once against semantic actions ("answer 2", "next")
  and run on all four bundles; a small per-device table turns an action into a
  key press or a tap.
- A bundle's SD card is a host directory, so progress files can be inspected
  and seeded directly.
- The simulator appears to model every RTC address as a PCF8563/BM8563. The
  X3's DS3231 would then read wrongly. Confirm in M0; either add a DS3231 model
  to the simulator or give sim builds a `Clock` backed by host time.

---

## 10. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| Content takes longer than firmware | v1 slips | Start authoring at M3; ship units incrementally; the pack format and validators exist before bulk writing |
| Content quality or wrong dialect | Teaches errors | Review gate enforced by the compiler (section 7.5); Peninsular lint; register tags |
| One reviewer for about 44 lessons | Review becomes the bottleneck | Printable per-lesson review pages; review in unit-sized batches from M3, not at the end |
| Bitmap strikes look small, or the outline headwords clash with the bitmap text | Poor readability | Judge on hardware in M1; text-size setting; fallback to 34 px headwords |
| URW-derived strikes cannot be redistributed | Lose 22 and 29 px sizes | Adobe-only scale is already defined |
| Fast-refresh ghosting from frequent card flips | Smeared text | Full refresh every N transitions, tunable; tune per controller on hardware |
| X4 Pro touch axis flips unconfirmed in the SDK | Taps land mirrored | Corner-tap test in M0; set flip flags once in `platform/Board` |
| Panel controller variants (five across the four devices) behave differently | Blank or poor display on some units | Use the facade only; test every variant that can be found; log the probe result on the About screen |
| Front key order and position versus footer cells | Labels do not line up with keys | Per-device key order and footer geometry in `ui/KeyMap`; confirm on each device in M0 |
| X4 has no clock | Wrong dates, broken streak | One-press date prompt at power-on; scheduler works in whole days; real clock options listed in section 6.8 |
| X4 Classic profile has pending items in the SDK (GPIO4 role, charge-status polarity, panel orientation) | Upside-down image or wrong charging icon | Check in M0; orientation is a `BoardProfile` field |
| Four devices to test | Regressions on the device not on the desk | Same scripted flows on four simulator bundles in CI; hardware checklist per device per milestone |
| Wrong S3 image flashed (Pro image on a Classic or the reverse) | Key pins driven as outputs, no display | Per-device file names; the About and boot log show the build's device; flashing guide warns |
| SD card missing, removed or corrupt | Progress lost | Journal-first writes, rebuild from journal, guest mode, storage error screen |
| RTC unset or reset | Wrong scheduling | Sanity check against last review and build date; ask for the date |
| GPIO0 is a boot strap and a side key on the X4 Pro and X4 Classic | Device enters download mode if held at reset | Document; never require that key at power-on |
| Platform release mismatch between SDK sample and CrossPoint | Build breaks | Pin one release in M0 |
| No audio | Pronunciation limited | Respelling, Unit 0, stated limitation |

---

## 11. Implementation plan

Milestones are ordered by dependency. Sizes are relative (S ≈ a few days,
M ≈ one to two weeks, L ≈ three weeks or more) and assume one developer.
Content work runs alongside from M3.

### M0 — Skeleton and bring-up (S)

- `git init`; add `freeink-sdk` as a submodule; `platformio.ini` with `x3x4`,
  `x4c` and `x4pro`; `partitions.csv`.
- `main.cpp` with the boot sequence from section 6.4; draw a test pattern that
  labels each front key's footer cell, and echo key and touch events.
- `tools/sim/build.sh` and `run.sh`; first golden screenshot for each of the
  four bundles.
- Hardware checks on each device: panel paints upright; every key maps
  correctly; the physical order of the front keys is recorded; X4 Pro
  corner-tap test fixes the touch flips; X4 Classic orientation and charge
  status are checked. Confirm the simulator RTC question.

**Done when:** all three envs build; the test pattern shows upright with
correctly labelled keys on all four simulator bundles and on each device on
hand; every key and a tap are logged correctly.

### M1 — Fonts and typography (S–M)

- `tools/sync-fonts.sh`, `tools/charset.py`, `tools/bdf2freeink.py`.
- Generate the curated strike set and the Termes headword strikes.
- `ui/Fonts` (slot plan), `core/text/Typesetter` with host tests.
- Specimen screen: every role, the full charset, accented capitals, a sample
  card.

**Done when:** the specimen renders identically in the simulator and on
hardware at both screen sizes; `¿Él está en Ávila? —Sí.` wraps and aligns on one baseline in mixed
styles; the Termes headword strikes are judged acceptable on hardware, or the
34 px fallback is chosen, and the result is recorded.

### M2 — App shell (M)

- `FreeInkApp` wiring; theme tokens per device, with the safe area taken from
  the board profile's bezel insets; status bar (battery, clock where there is
  one).
- `KeyMap` (choice bar versus focus routing, per-device key order); pause
  sheet; Home; Settings.
- Refresh policy (async present, hint accumulation, full-refresh cadence).
- `platform/Clock` with its two backends (RTC; asked date on the X4), the
  set-clock screen and the "What day is it?" screen; `platform/Storage`;
  `profile.bin`.
- Sleep and wake with a static sleep screen; idle timeout. Measure sleep
  current on each device and record what stays powered.
- X4 Pro light sheet (`FrontlightManager`, brightness and warmth).

**Done when:** a user can move through Home and Settings with keys only on the
X3, X4 and X4 Classic and with touch only on the X4 Pro; settings survive
sleep; the X4 schedules correctly across a scripted week of date prompts; no
press is lost during a refresh in a scripted 50-press simulator test; wake to
screen is under 2 s.

### M3 — Content pipeline and pack (M)

- `docs/pack-format.md`; `packc` with tokeniser, conjugator, respeller,
  validators, `ids.lock`.
- `core/pack` reader with host tests; `.incbin` embedding; disk loading for
  host and simulator.
- Sample content: Units 0–1, about 150 lemmas, 200 sentences.
- Dictionary browse and entry page with verb table.

**Done when:** `packc --check` passes on the sample and fails on seeded errors
(bad character, `vosotros` form, missing example); the device shows a
dictionary entry and a verb table straight from the embedded pack; heap use is
unchanged by pack size.

### M4 — Scheduler and flashcards (M)

- `core/srs/Fsrs` with reference vectors; `ItemState`; `DayQueue`.
- Progress store: `items.bin`, `reviews.log`, replay and rebuild, guest mode.
- Session controller; flashcard views (both directions); grade footer with
  interval previews; undo; session resume after sleep.
- Progress screen: streak, totals, 14-day forecast.

**Done when:** the FSRS port matches the reference on all vectors; a simulated
30-day run with the virtual clock produces the expected due counts; pulling the
SD card or cutting power mid-session loses at most the review in flight.

### M5 — Exercises and lessons (M–L)

- Multiple choice, cloze, gender, conjugation, word order; distractor
  generator; answer checking.
- Lesson flow: note pages → presentation → practice → summary; course map;
  unlock rules.
- Today session composition; exercise picker by maturity and device.
- Content through Unit 3.

**Done when:** a new user can complete Units 0–3 on any of the four devices;
every exercise is answerable with one press on key devices (word order
excepted); scripted simulator runs cover each exercise type on all four
bundles; Units 0–3 are reviewed.

### M6 — Reader, phrasebook, sleep screen (M)

- Reader with pagination, word cursor (key devices), tap-to-gloss (X4 Pro),
  sentence translation, add-to-deck, comprehension questions.
- Phrasebook with categories and "practise this category".
- Dictionary search with the on-screen Spanish keyboard (X4 Pro) and
  letter-jump (key devices); English → Spanish lookup.
- Sleep screen with a weak word; optional timed rotation on the X4 Classic and
  X4 Pro after measuring sleep current.

**Done when:** a dialogue can be read and every word glossed on all four devices;
starred words appear in the next session; sleep current and rotation cost are
measured and recorded.

### M7 — Full course and release (L)

- Content Units 4–12, frequency deck, full phrasebook and dictionary; every
  lesson reviewed.
- UI in Spanish (immersion setting); first-run flow; About and licences.
- Usage log and `tools/usage-report.py` (section 8.6).
- `RecoveryBoot` for SD firmware update; USB mass-storage mode on the X4 Pro
  and X4 Classic for backing up `/tinta/`.
- Hardware soak: a week of daily use on each device; ghosting and battery
  tuning; three release images and per-device flashing instructions.

**Done when:** the full pack passes validation and every lesson carries a
current review hash; each device on hand completes a seven-day soak with no
lost progress; all three images fit the app slot with margin; licences are
settled.

### M8 — After v1

- BLE keyboard for typed answers (SDK `BleKeyboardHost`).
- A real clock for the X4: NTP over Wi-Fi at wake, or true deep sleep.
- SD expansion packs; Anki import tool on the PC.
- B1 content (subjunctive, conditional, future).
- PC tool to refit FSRS weights from `reviews.log`.
- Match-pairs exercise on touch.

### Dependency order

```
M0 ─► M1 ─┬─► M2 ─┬─► M4 ─► M5 ─► M6 ─► M7
          └─► M3 ─┘
              └─ content authoring ──────► M7
```

M3 can start as soon as M1's charset exists; M4 needs M2 (shell, storage) and
M3 (pack). Content authoring starts with M3's sample and runs until M7.

---

## 12. Optional SDK changes

None is required. Each removes a workaround, and the SDK checkout is local.

| Change | Removes |
|---|---|
| Sparse codepoint ranges in `BitmapFont` | The C1 slot remap (section 5.4) |
| Configurable `DisplayTarget::FONT_SLOTS`, or a public "draw run at baseline with this font" call | The scratch-slot technique (section 5.5) |
| DS3231 model in the simulator | The sim-only clock fallback |
| `build-firmware.sh` option to add SDK sources | Part of `tools/sim/build.sh` |
| Confirmed X4 Pro `flipX`/`flipY` in the board profile | The local override in `platform/Board` |
| A boot-time detector for X4 Classic versus X4 Pro | The separate `x4c` and `x4pro` builds (one S3 image) |

---

## 13. Decisions and open questions

Decided (2026-10-03):

- **Name:** Tinta.
- **Content review:** the owner's wife reviews all course content.
- **Devices:** X3, X4, X4 Classic and X4 Pro are all supported targets.
- **Devices on hand:** X3, X4 Classic and X4 Pro. The original X4 is covered
  by the simulator only and is marked as such in release notes.
- **SDK checkout:** `freeink-sdk` is a symlink to `../freeink-sdk` during
  development, with the same paths a submodule would have. Converting it to a
  submodule is a release step.
- **Headword strikes:** rasterised from TeX Gyre Termes Bold (50–87 px), after
  trying Scale2x and hqx scaling of the bitmap strikes. Adobe bitmaps stay for
  34 px and all body text.
- **Platform pin:** pioarduino `55.03.311`, the release CrossPoint builds on
  and the one already installed.

Open. Defaults are what the plan assumes; none blocks M0.

1. **Hardware checks.** Agents cannot run hardware. Each milestone leaves a
   checklist in `docs/hardware-notes.md` for the three devices on hand.
2. **X4 date prompt.** Default: ask for the day at each power-on (section 6.8).
   The alternatives need Wi-Fi or a sleep-current measurement.
3. **Content licence.** Default: CC BY-SA if Wiktionary data is used.
4. **CrossPoint 22 and 29 px strikes.** Default: use them; resolve the URW
   terms before any public release.
5. **SDK changes.** Default: none; raise the optional ones in section 12
   separately.
6. **Level.** Default: A1–A2 in v1, B1 after.
7. **Grades.** Default: four (Again, Hard, Good, Easy). A two-grade mode is a
   small addition if preferred.

---

## Appendix A — Facts this plan relies on

Read from the local checkouts on 2026-10-03.

**FreeInk SDK**

- `README.md` — device table; X3 and X4 share one C3 binary via
  `selectXteinkDevice()` and `setDisplayX3()`; per-batch controllers resolved
  by `applyXteinkDisplayController()`; capability flags; PlatformIO `lib_deps`.
- `platformio.sample.ini` — `xteink`, `x4c` and `x4pro` envs, platform pin.
- `docs/xteink-x4pro-support.md` — pins, GT911 (`swapXY`, flips pending), Home
  pad, keys on GPIO0/7/3, frontlight on GPIO8/9, SDMMC, BM8563, CW2017.
- `docs/xteink-x3-uc8279-support.md` — UC8279d variant and boot probe.
- `docs/xteink-x4c-support.md` — X4 Classic: same S3 board and glass as the
  Pro, different display pins, seven discrete keys (side keys on GPIO0/7,
  bottom keys on GPIO5/2/8/9), no touch or frontlight, controller chosen from
  the factory NVS value, pending items (GPIO4, charge status, orientation).
- `libs/hardware/BoardConfig/include/BoardConfig.h` — `XTEINK_X3` (792×528, ADC
  ladder, BQ27220, DS3231, no touch, frontlight or audio), `XTEINK_X4`
  (800×480, ADC ladder, ADC battery, no RTC or other sensors),
  `XTEINK_X4_CLASSIC` and `XTEINK_X4_PRO` (bezel insets, BM8563, CW2017).
- `docs/freeink-ui.md`, `libs/ui/FreeInkUI/include/FreeInkUIDisplayTarget.h`,
  `FreeInkUIFont.h` — `FreeInkApp`, `DisplayTarget` (final, eight font slots,
  U+2026 handling, glyph bit packing, baseline at `rect.y + ascent`),
  `BitmapFont` (contiguous range, 16-bit bitmap offset), `presentAsync`,
  `qwertyKeyboard` with `SpanishEs`, `snapshotFrom`.
- `libs/hardware/*/include` — `InputManager` (keys, touch, Home pad, async
  queue), `PowerManager`, `Rtc`, `SDCardManager`, `FrontlightManager`,
  `UsbMassStorage`, `RecoveryBoot`.
- `docs/simulator.md`, `tools/simulator/build/build-firmware.sh`,
  `tools/simulator/core/I2cDevices.cpp` — bundle builds, CLI, which SDK sources
  are compiled, RTC model.

**CrossPoint reader**

- `x11/font-adobe-100dpi-1.0.4/`, `x11/font-crosspoint-100dpi/` (and its
  `README.md`) — the strikes, their sizes, provenance and licence notes.
- `lib/EpdFont/scripts/convert-builtin-fonts.sh`, `lib/EpdFont/EpdFontData.h`,
  `lib/EpdFont/builtinFonts/*.h` — how CrossPoint builds its font headers and
  why they are large.
- `platformio.ini`, `partitions.csv` — separate `x4pro` and `x4c` envs and
  their settings, partition table.
- `lib/hal/HalGPIO.cpp`, `src/main.cpp` — device detection order at boot.
- `lib/hal/HalPowerManager.cpp`, `lib/hal/HalClock.h` — sleep path (GPIO13
  battery cut on the C3 boards, latch held on the S3 boards); the clock exists
  only where the SDK finds an RTC.
- `AGENTS.md` — C3 RAM figure and memory rules.

Glyph coverage and subset sizes in section 5 were measured directly from the
BDF files.

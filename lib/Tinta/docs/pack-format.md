# Tinta course pack format

Format version **1.1**. Written by `tools/packc` (`emit.py`), read by
`src/core/pack/Pack` on the device and by `tools/packc/reader.py` for dumps and
tests. This document is the contract between the two; change it first.

The pack is "smart compiler, dumb device": everything linguistic (tokenising,
lemma linking, conjugation, respelling, item generation, distractor choice) is
done by the compiler. The device reads fixed-size records with bounds-checked
index arithmetic and binary searches. It never allocates and never copies a
string: a string is a `const char*` into the pack.

## 1. General rules

- **Byte order.** Little-endian. The reader supports little-endian hosts only
  (ESP32-C3/S3 and every host we build on) and checks this at open.
- **Alignment.** The header, the directory and every section start at a
  multiple of 4 bytes from the start of the pack. Every record size is a
  multiple of 4 and every field is naturally aligned inside its record, so a
  record never straddles an alignment boundary. Sections are separated by
  zero padding; the pack size is a multiple of 4.
- **Reading.** The reader copies each record into a local struct with
  `memcpy` before using it. This makes reads independent of the base pointer's
  alignment (the RISC-V C3 traps on some unaligned loads) and of C++ aliasing
  rules. Records are 4–176 bytes, so the copies are cheap and stay well under
  the 256-byte local limit.
- **Strings.** NUL-terminated UTF-8 in the Tinta font charset
  (`tools/charset.py`): U+0020–U+007E, U+00A0–U+00FF, the C1 slots
  U+0080/U+0091–U+0097 for `€ ‘ ’ “ ” • – —`, U+2026 `…` (drawn as three dots
  by `DisplayTarget`) and `\n` inside multi-line strings. All strings live in
  one deduplicated heap (`STRS`) and are referenced by a `u32` byte offset into
  that heap. **Offset 0 is the empty string** and means "none".
- **Ids.** Records are addressed by their index in their section ("lemma id",
  "sentence id", ...). Ids are 16-bit; `0xFFFF` (`NONE16`) means "none". The
  compiler fails if a section would exceed 65,535 records. Ids are only valid
  for one pack build; persistent learner state uses item `uid`s (section 3.9).
- **Folded keys.** Search keys are ASCII produced by the fold function of
  section 2. Sorted tables compare keys bytewise (`strcmp`).
- **Unused fields** marked `reserved` are written as zero and ignored by
  readers.

## 2. Folding

`fold(text)` maps text to a search key. The Python implementation is
`tools/packc/fold.py`; the C++ one is `tinta::core::foldKey` in
`src/core/pack/Fold.h`. Host tests compare the two over every codepoint of the
charset (`fold.py --vectors`).

1. Decode UTF-8 to codepoints (Python: NFC first).
2. Map each codepoint:
   - `A`–`Z` → lower case; `a`–`z` and `0`–`9` → themselves;
   - Latin-1 letters lose their diacritics and fold case:
     `ÀÁÂÃÄÅàáâãäå` → `a`, `Ææ` → `ae`, `Çç` → `c`, `ÈÉÊËèéêë` → `e`,
     `ÌÍÎÏìíîï` → `i`, `Ðð` → `d`, `Ññ` → `n`, `ÒÓÔÕÖØòóôõöø` → `o`,
     `ÙÚÛÜùúûü` → `u`, `Ýýÿ` → `y`, `Þþ` → `th`, `ß` → `ss`;
   - space, tab and newline → a separator;
   - everything else (punctuation, `¿ ¡ ª º`, C1 slots, any codepoint above
     U+00FF) is dropped.
3. Runs of separators become one space; leading and trailing separators are
   removed.

Examples: `¿Qué onda?` → `que onda`, `Niño` → `nino`, `año` → `ano`,
`e-mail` → `email`, `¡Órale!` → `orale`.

Folding is many-to-one (`año`/`ano`, `está`/`esta`), so every lookup returns a
range of matches, and the record keeps the exact form for display.

## 3. Layout

```
+--------------------+  0
| Header (48 bytes)  |
+--------------------+  48 = directoryOffset
| Directory          |  sectionCount × 16 bytes
+--------------------+
| Sections ...       |  each 4-aligned, zero padding between them
+--------------------+  size
```

### 3.1 Header

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | `char[4]` | `magic` | `"TNTA"` |
| 4 | `u16` | `formatMajor` | `1` |
| 6 | `u16` | `formatMinor` | `1` |
| 8 | `u32` | `contentVersion` | from `content/course.yaml` `version` |
| 12 | `u32` | `buildTime` | Unix seconds, UTC; `SOURCE_DATE_EPOCH` if set |
| 16 | `u32` | `size` | total pack bytes, header included |
| 20 | `u32` | `crc32` | CRC-32 of the whole pack with this field read as zero |
| 24 | `char[8]` | `locale` | `"es-MX"`, NUL-padded |
| 32 | `u32` | `directoryOffset` | `48` |
| 36 | `u16` | `sectionCount` | |
| 38 | `u16` | `headerSize` | `48`; a reader skips header bytes it does not know |
| 40 | `u32` | `flags` | bit 0 `RELEASE`: built with `--release` (every reviewable file carries a matching review hash) |
| 44 | `u32` | `reserved` | |

CRC-32 is the IEEE 802.3 polynomial (reflected `0xEDB88320`, initial value
and final XOR `0xFFFFFFFF`), i.e. Python's `zlib.crc32`.

### 3.2 Directory

`sectionCount` entries of 16 bytes:

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | `u32` | `tag` | four ASCII characters, first character in the lowest byte (`"LEMM"` is `0x4D4D454C`) |
| 4 | `u32` | `offset` | from the start of the pack, multiple of 4 |
| 8 | `u32` | `size` | payload bytes, padding excluded |
| 12 | `u32` | `count` | number of records (for `STRS`: number of distinct strings) |

For record sections the **stride** is `size / count` (0 when `count` is 0).
A reader requires `size == count × stride` and `stride ≥` the record size it
knows, and reads records at `offset + index × stride`. That lets a later minor
version append fields to a record without breaking older readers.

### 3.3 Sections

| Tag | Record | Size | Contents | Access |
|---|---|---|---|---|
| `STRS` | bytes | – | string heap | by offset |
| `LEMM` | `Lemma` | 48 | lemmas, sorted by folded headword | by lemma id |
| `LKEY` | `LemmaKey` | 8 | folded headword → lemma, sorted | binary search, prefix range, alphabetical browse |
| `EKEY` | `EnglishKey` | 8 | folded English keyword → lemma, sorted | binary search, prefix range |
| `FORM` | `FormKey` | 12 | folded inflected form → lemma and form tag, sorted | binary search |
| `VERB` | `VerbTable` | 176 | conjugations | by verb-table index |
| `SENT` | `Sentence` | 24 | sentences | by sentence id |
| `TOKS` | `Token` | 8 | tokens, grouped by sentence | by `Sentence.firstToken + i` |
| `ITEM` | `Item` | 20 | schedulable items, in teaching order | by item index |
| `IUID` | `ItemUid` | 8 | uid → item index, sorted by uid | binary search |
| `DIST` | `u32` | 4 | distractor candidates, grouped by item | by `Item.firstCandidate + i` |
| `UNIT` | `Unit` | 20 | units in course order | by unit index |
| `LESS` | `Lesson` | 32 | lessons in course order | by lesson id |
| `NOTE` | `Note` | 16 | grammar, culture, pronunciation and usage notes | by note id |
| `NSPN` | `NoteSpan` | 8 | styled text spans of the notes | by `Note.firstSpan + i` |
| `STOR` | `Story` | 24 | dialogues and readings | by story id |
| `SLIN` | `StoryLine` | 8 | story lines | by `Story.firstLine + i` |
| `SQST` | `Question` | 24 | comprehension questions | by `Story.firstQuestion + i` |
| `PHRS` | `PhraseCategory` | 12 | phrasebook categories | by category id |
| `PENT` | `PhraseEntry` | 8 | phrasebook entries, grouped by category | by `PhraseCategory.firstEntry + i` |
| `CONF` | `ConfusableSet` | 24 | authored distractor sets | linear scan (tens of records) |
| `LEXS` | `u16` | 2 | example sentence ids, grouped by lemma | by `Lemma.firstExample + i` |

All 22 sections are required in format 1.x (any of them may have `count` 0).
A reader ignores sections whose tag it does not know.

Compared with the sketch in PLAN.md §7.3: `UNIT`, `NSPN`, `SLIN`, `SQST`,
`PENT`, `DIST` and `LEXS` are split out of the section they belong to so that
every section holds one fixed-size record type; `Lemma` and `Item` grew to
hold a lemma's plural, feminine and accepted synonyms, and an item's
prerequisite and distractor candidates.

### 3.4 Enumerations

**Part of speech** (`Lemma.pos`): 1 noun, 2 verb, 3 adjective, 4 adverb,
5 pronoun, 6 determiner, 7 preposition, 8 conjunction, 9 interjection,
10 numeral, 11 expression (multi-word lemma), 12 proper noun.

**Gender** (`Lemma.gender`): 0 none, 1 masculine, 2 feminine, 3 both
(`el/la estudiante`, or a noun with a feminine form such as `amigo/amiga`).

**Level**: 1 A1, 2 A2, 3 B1, 4 B2; 0 unknown (dictionary-only headwords, 1.1).

**Register** (`Lemma.reg`, `Sentence.reg`): 0 neutral, 1 formal, 2 informal,
3 vulgar. Vulgar content is hidden unless enabled in settings.

**Form tag** (`u16`, `Token.tag`, `FormKey.tag`, `Item.b` of conjugation
items):

| Range | Meaning |
|---|---|
| `0x0000` | the headword itself (or unknown) |
| `0x1000 \| tense << 4 \| person` | finite verb form; `| 0x0100` when the token carries attached pronouns (`dígame`) |
| `0x2000 \| n` | non-finite: 0 infinitive, 1 gerund, 2 participle (m. sg.), 3 participle f. sg., 4 participle m. pl., 5 participle f. pl. |
| `0x3000 \| gender << 4 \| number` | noun/adjective inflection: gender 0 unmarked, 1 m, 2 f; number 0 singular, 1 plural (`0x3001` plural, `0x3020` feminine, `0x3021` feminine plural, `0x3011` masculine plural) |
| `0x4001` | apocope (`buen`, `gran`, `primer`) |

Tenses: 0 present, 1 preterite, 2 imperfect, 3 future, 4 conditional,
5 present subjunctive, 6 imperative (affirmative), 7 negative imperative.
Persons: 0 `yo`, 1 `tú`, 2 `él/ella/usted`, 3 `nosotros`, 4
`ellos/ellas/ustedes`. There is no `vosotros` person. Imperatives exist for
persons 1, 2 and 4 only.

Source markup writes tags as `pres.3s`, `pret.1s`, `impf.1p`, `fut.3p`,
`cond.2s`, `subj.3s`, `imp.2s`, `impneg.3p`, `inf`, `ger`, `part`, `part.f`,
`part.mpl`, `part.fpl`, `pl`, `f`, `fpl`, `mpl`, `apoc`
(see `docs/content-style.md`).

### 3.5 `LEMM` — `Lemma` (48 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | `u32` | `es` | headword without article (`chamba`, `por favor`, `¿mande?`) |
| 4 | `u32` | `en` | English gloss; senses separated by `; ` |
| 8 | `u32` | `pron` | respelling, stressed syllable in capitals (`CHAHM-bah`) |
| 12 | `u32` | `note` | usage note (English), 0 = none |
| 16 | `u32` | `alt` | accepted non-Mexican synonyms, `; `-separated, 0 = none |
| 20 | `u32` | `feminine` | feminine singular form (adjectives, `amigo` → `amiga`), 0 = none |
| 24 | `u32` | `plural` | plural (masculine plural for adjectives), 0 = none or invariable |
| 28 | `u32` | `firstExample` | index into `LEXS` |
| 32 | `u16` | `exampleCount` | |
| 34 | `u16` | `freqRank` | 1 = most frequent, 0 = unranked |
| 36 | `u16` | `lesson` | lesson that introduces the lemma, `NONE16` if none |
| 38 | `u16` | `verbTable` | index into `VERB`, `NONE16` if not a verb |
| 40 | `u8` | `pos` | |
| 41 | `u8` | `gender` | |
| 42 | `u8` | `level` | |
| 43 | `u8` | `reg` | register |
| 44 | `u16` | `flags` | see below |
| 46 | `u16` | `reserved` | |

Flags: bit 0 `MEXICO` (Mexico-specific word or sense), bit 1 `PLURAL_ONLY`
(`los lentes`), bit 2 `EL_FEMININE` (feminine noun that takes `el` in the
singular: `el agua`), bit 3 `HAS_ITEMS` (the lemma has vocabulary items),
bit 4 `FREQUENCY_DECK`, bit 5 `CONFUSABLE` (member of a `CONF` set), bit 6
`DICTIONARY_ONLY` (1.1: from `lexicon/dictionary*.tsv`; searchable, with no
items and no examples required, never offered as an option).

Lemma ids are assigned in `LKEY` order (folded headword, then exact headword,
then part of speech), so lemma id order is alphabetical order.

Examples (`LEXS[firstExample .. +exampleCount)`) list sentences where the lemma
is the marked target first, in course order, then other sentences that
contain it; at most 8.

### 3.6 Keys: `LKEY`, `EKEY`, `FORM`

```
LemmaKey   (8):  u32 key; u16 lemma; u16 reserved;
EnglishKey (8):  u32 key; u16 lemma; u8 rank; u8 reserved;
FormKey   (12):  u32 key; u32 form; u16 lemma; u16 tag;
```

- `LKEY` has exactly one entry per lemma (its folded headword), sorted by
  (`key`, `lemma`). Its index order is the alphabetical dictionary list; a
  prefix search gives a contiguous index range for search-as-you-type and the
  letter-jump menu.
- `EKEY` is built from glosses: each sense (split on `;` and `,`, without
  parenthesised text and a leading `to`, `a`, `an` or `the`) is a key with
  rank 0, and each word of a multi-word sense that is not an English stop word
  is a key with rank 1. Sorted by (`key`, `rank`, `lemma`); a lemma appears at
  most once per key.
- `FORM` holds every generated inflection (all verb-table forms, with the
  pronoun of a reflexive verb removed, non-finite forms, plurals, feminines)
  and every token surface seen in a sentence, except forms equal to the
  headword (those are in `LKEY`). `form` is the exact spelling for display.
  A spelling that fills several cells of one verb's table (`hable`: subj.1s,
  subj.3s, imp.3s, impneg.3s) is listed once, under its first tag; the UI
  finds the other cells by scanning that verb's `VERB` row.
  Sorted by (`key`, `lemma`, `tag`).

### 3.7 `VERB` — `VerbTable` (176 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | `u32[8][5]` | `forms` | `forms[tense][person]`, 0 = no such form |
| 160 | `u32` | `infinitive` | |
| 164 | `u32` | `gerund` | |
| 168 | `u32` | `participle` | masculine singular |
| 172 | `u16` | `lemma` | |
| 174 | `u16` | `flags` | see below |

Reflexive verbs include their pronoun: `me llamo`, `llámate`, negative
imperative `te llames`, gerund `llamándose`. Negative imperatives are stored
without `no`; the UI prints `no` before them.

Flags: bit 0 `IRREGULAR` (has entries in `content/verbs/irregular.yaml`
beyond a stem change), bits 1–3 stem-change class (0 none, 1 `e→ie`,
2 `o→ue`, 3 `e→i`, 4 `u→ue`, 5 `i→ie`), bit 4 `SPELLING` (orthographic change
such as `buscar` → `busqué`), bit 5 `REFLEXIVE`.

### 3.8 `SENT` and `TOKS`

```
Sentence (24):
  0  u32 es            Spanish text, markup removed
  4  u32 en            English translation
  8  u32 note          optional note (English), 0 = none
  12 u32 firstToken    index into TOKS
  16 u16 lesson        NONE16 if not from a lesson
  18 u8  tokenCount
  19 u8  level
  20 u8  source        0 lesson sentence, 1 dialogue line, 2 story line, 3 phrasebook entry,
                       4 frequency-deck sentence (1.1, content/deck/*.yaml)
  21 u8  flags         bit 0 HAS_TARGET, bit 1 WORD_ORDER (has a word-order item)
  22 u8  reg           highest register among its lemmas
  23 u8  reserved

Token (8):
  0  u16 start         byte offset of the token in Sentence.es
  2  u8  length        bytes
  3  u8  flags         bit 0 TARGET (marked in the source), bit 1 MULTIWORD,
                       bit 2 NUMBER (digits, no lemma), bit 3 NAME (proper noun)
  4  u16 lemma         NONE16 for numbers
  6  u16 tag           form tag
```

Tokens are words only, in text order, never overlapping. Punctuation and
spaces between tokens are not tokens. A multi-word token (`por favor`,
`con permiso`) spans its spaces. Every word of every sentence is a token, so
the reader can gloss any word without stemming.

### 3.9 `ITEM`, `IUID`, `DIST`

```
Item (20):
  0  u32 uid             stable id from content/ids.lock (never reused)
  4  u8  kind            ItemKind (src/core/ItemCatalog.h)
  5  u8  candidateCount
  6  u16 lesson          NONE16 for frequency-deck and phrasebook items
  8  u16 a               operand, see below
  10 u16 b               operand, see below
  12 u16 prereq          item index that must be learnt first, NONE16 if none
  14 u8  flags           bits 0-1: leading candidates that must be shown
                         (authored confusables); bit 2 FREQUENCY_DECK;
                         bit 3 PHRASEBOOK
  15 u8  reserved
  16 u32 firstCandidate  index into DIST

ItemUid (8):  u32 uid; u16 index; u16 reserved;      sorted by uid
```

| Kind | `a` | `b` | `DIST` candidates | `prereq` |
|---|---|---|---|---|
| 0 `VocabRecognise` | lemma | 0 | lemma ids (show their `en`) | none |
| 1 `VocabProduce` | lemma | 0 | lemma ids (show their `es`) | the lemma's recognise item |
| 2 `Cloze` | sentence | token index in the sentence | string offsets: option text, inflected and capitalised to fit the gap | recognise item of the token's lemma, if any |
| 3 `Conjugation` | lemma (verb) | form tag | string offsets: other forms of the verb | the verb's recognise item, if any |
| 4 `Gender` | lemma (noun) | 0 | none | the noun's recognise item |
| 5 `Phrase` | sentence | phrasebook entry id | sentence ids of other phrases | none |
| 6 `WordOrder` | sentence | 0 | none | none |

Items are in teaching order, and the scheduler introduces new items in item
index order:

1. **Lesson items**, lesson by lesson. Inside a lesson: first the
   `VocabRecognise` items of the lesson's new lemmas in authored order (there
   are exactly `Lesson.newCount` of them), then gender, produce, conjugation,
   cloze and word-order items. `Item.lesson` is the lesson id; a lesson's
   items are the contiguous range `Lesson.firstItem .. +itemCount`.
2. **Frequency-deck items** (`flags` bit 2, `lesson = NONE16`): the deck
   words' recognise items in frequency-rank order, then their gender and
   produce items, then cloze and word-order items from the deck sentences
   (`content/deck/*.yaml`, sentence source 4) in the frequency order of the
   words they practise; each deck word's recognise item gates its produce and
   cloze items. They belong to no lesson: the
   scheduler introduces them only when the unlocked lessons have no new items
   left for the day. Through `ItemCatalog` they are the items with
   `lessonAt() == 0xFFFF` and `kindAt() != Phrase`.
3. **Phrasebook items** (`flags` bit 3, kind `Phrase`, `lesson = NONE16`). They
   are introduced only when the learner practises that phrasebook category,
   never by the daily session.

**Uids** come from `content/ids.lock`: they start at 1, are never reused, and
`0xFFFFFFFF` is never assigned (the progress store uses it to mark damaged
records). Item keys in `ids.lock` describe what an item asks, not where it
stands (`cloze:la mesa es nueva:mesa`), so reordering content keeps uids
(`docs/content-style.md`, section 12).

Candidates are ordered best first. The device picks up to three with a
deterministic generator seeded by uid and repetition count; the leading
`flags & 3` candidates (members of an authored confusable set) are always
among them. Candidates are pairwise distinct as displayed and pairwise
unrelated in meaning, so any subset is a fair question. Every multiple-choice
kind has at least three candidates, **except a cloze on a closed-class word**
(determiner, pronoun, preposition, conjunction) or on a verb form (whose
options are the same verb's forms in the tenses taught so far), which may have
one or two: `este` has only `ese` and `aquel`. A multiple-choice screen therefore shows
**two to four options** (`candidateCount` 1–8, of which at most three are
shown, plus the answer).

### 3.10 `UNIT`, `LESS`, `NOTE`, `NSPN`

```
Unit (20):
  0  u32 title           Spanish
  4  u32 titleEn
  8  u32 goals           English, one goal per line ('\n')
  12 u16 number          as authored (0 = Unit 0)
  14 u16 firstLesson
  16 u16 lessonCount
  18 u16 reserved

Lesson (32):
  0  u32 title           Spanish
  4  u32 titleEn
  8  u32 firstItem       the lesson's items are ITEM[firstItem .. +itemCount)
  12 u16 itemCount
  14 u16 unit            unit index
  16 u16 number          lesson number inside the unit, as authored
  18 u16 firstNote
  20 u16 noteCount
  22 u16 dialogue        story id, NONE16 if none
  24 u16 firstSentence   the lesson's sentences are SENT[firstSentence .. +sentenceCount)
  26 u16 sentenceCount
  28 u16 newCount        new lemmas = lemmas of the first newCount items
  30 u8  level
  31 u8  flags           bit 0 REVIEWED (review hash matches the content)

Note (16):
  0  u32 title           English
  4  u32 firstSpan       index into NSPN
  8  u16 spanCount
  10 u16 lesson
  12 u8  kind            0 grammar, 1 culture, 2 pronunciation, 3 usage
  13 u8  flags
  14 u16 reserved

NoteSpan (8):
  0  u32 text            0 for break styles
  4  u8  style
  5  u8  flags
  6  u16 reserved
```

A lesson's `(unit number, lesson number)` pair is its stable identity across
pack versions; the lesson id is not.

Span styles: 0 `TEXT` (English body), 1 `STRONG` (English bold), 2 `SPANISH`
(Spanish example, Times), 3 `SPANISH_STRONG` (Times Bold), 4 `RESPELLING`
(Helvetica Oblique), 5 `NOT_MEXICAN` (a Peninsular or other non-Mexican form
quoted for contrast; the UI marks it as such), 6 `PARAGRAPH` (paragraph
break), 7 `BULLET` (paragraph break that starts a bullet item). Spans keep
their inner spaces; the typesetter wraps them as one flow per paragraph.

### 3.11 `STOR`, `SLIN`, `SQST`

```
Story (24):
  0  u32 title           Spanish
  4  u32 titleEn
  8  u32 firstLine       index into SLIN
  12 u16 lineCount
  14 u16 lesson          NONE16 for standalone readings
  16 u16 firstQuestion   index into SQST
  18 u8  questionCount
  19 u8  level
  20 u8  kind            0 dialogue, 1 reading
  21 u8  flags
  22 u16 reserved

StoryLine (8):
  0  u32 speaker         0 = narration
  4  u16 sentence
  6  u16 flags           bit 0 NEW_PARAGRAPH

Question (24):
  0  u32 text
  4  u32 options[4]      0 = unused
  20 u8  answer          index of the right option
  21 u8  optionCount
  22 u8  flags           bit 0 SPANISH (question and options are Spanish)
  23 u8  reserved
```

Lesson dialogues are stories of kind 0 with `lesson` set.

### 3.12 `PHRS`, `PENT`, `CONF`, `LEXS`, `DIST`

```
PhraseCategory (12):  u32 title; u32 titleEn; u16 firstEntry; u16 entryCount;
PhraseEntry (8):      u32 pron; u16 sentence; u16 category;
ConfusableSet (24):   u32 name; u32 note; u16 members[6]; u16 memberCount; u16 reserved;
LEXS:                 u16 sentence ids
DIST:                 u32 candidates (meaning depends on the item kind, 3.9)
```

`ConfusableSet.members` are lemma ids; unused slots are `NONE16`.

## 4. Validation by the reader

`Pack::open(data, size)` is O(number of sections) and checks: size ≥ 48;
magic; `formatMajor == 1`; `headerSize ≥ 48`; `header.size == size`; the
directory fits; every section is 4-aligned and inside the pack; each required
section is present once; record sections satisfy `size == count × stride`
with `stride ≥` the known record size; `STRS` is non-empty, starts with NUL
and ends with NUL. Accessors bounds-check every id and string offset, so a
structurally valid pack cannot make the reader read outside it.

`Pack::verifyCrc()` checks the CRC over the whole pack (about 2 MB; call it
once, for example at first boot or from a diagnostics screen).

## 5. Versioning

- `formatMajor` changes when an existing field changes meaning, size or
  position, a record shrinks, or a section is removed. A reader rejects any
  other major version.
- `formatMinor` changes for additive changes: a new section, a field appended
  to a record (readers use the directory stride), or a meaning given to a
  `reserved` field or unused flag bit whose zero value keeps the old meaning.
  A reader accepts any minor version of its major and ignores what it does not
  know; code that needs a newer minor checks `formatMinor()` or the presence
  of the section.
- `contentVersion` is the course edition, independent of the format. Learner
  progress is keyed by item uid (`content/ids.lock`), so any content version
  can be loaded over existing progress.
- History: 1.0 first format; 1.1 sentence source 4 (deck), lemma flag
  bit 6 (dictionary only), level 0 (unknown).
- Bumping either format number means updating this document,
  `tools/packc/emit.py`, `tools/packc/reader.py` and
  `src/core/pack/PackFormat.h` together.

## 6. The reader (`src/core/pack/`)

Full declarations and comments are in `Pack.h`, `PackFormat.h` and `Fold.h`;
this is the shape of the API for UI code.

- **Records**: `PackFormat.h` declares one struct per record of section 3
  (`Lemma`, `Sentence`, `Token`, `Item`, `Lesson`, ...) with the field names
  used here, plus the enums (`PartOfSpeech`, `Gender`, `Register`, `Tense`,
  `Person`, `SpanStyle`, ...), the flag constants (`kLemmaMexico`,
  `kTokenTarget`, ...) and form-tag helpers (`makeVerbTag`, `isVerbTag`,
  `tagTense`, `tagPerson`). Namespace `tinta::core::pack`.
- **Opening**: `Pack pack; PackStatus s = pack.open(data, size);` checks the
  structure only (section 4); `pack.verifyCrc()` is the full check. `Pack` is
  about 450 bytes: keep it static. It implements `tinta::core::ItemCatalog`.
- **Strings**: `pack.str(offset)` returns a `const char*` into the pack ("" for
  0 or a bad offset). Hand it to FreeInkUI as is.
- **Records**: `bool lemma(uint16_t id, Lemma& out)` and the same for
  `sentence`, `item`, `unit`, `lesson`, `note`, `story`, `phraseCategory`,
  `confusable`; children through their parent: `token(sentence, i, out)`,
  `noteSpan(note, i, out)`, `storyLine(story, i, out)`,
  `question(story, i, out)`, `phraseEntry(category, i, out)`; also by global
  index. Each returns false (and a zeroed struct) for an id out of range.
  `count(Section::...)` gives every section's record count.
- **Helpers**: `lemmaExample(lemma, n)` (sentence id), `itemCandidate(item, n)`
  (meaning by kind, section 3.9), `tokenText(sentence, token)` (pointer and
  byte length of a word), `verbInfo(table, out)`,
  `verbForm(table, tense, person)` and `verbForm(table, tag)`.
- **Search**: `findLemma(query)` (exact), `findLemmaPrefix(query)` (as you
  type; "" = all, which is also the alphabetical list), `findForm(query)`,
  `findEnglish(query)`, `findEnglishPrefix(query)` fold the query (section 2)
  and return a `KeyRange {first, count}` of indexes; read entries with
  `lemmaKeyAt`, `formAt`, `englishAt`. No allocation; queries are folded into
  a 64-byte local.
- **Folding**: `tinta::core::foldKey(utf8, out, outSize)` in `Fold.h`, for any
  other key the UI builds.

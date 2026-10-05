# Tinta content style guide

The working manual for everyone who writes or reviews Tinta course content:
the agents that draft lessons, and the native Mexican Spanish speaker who
reviews them. It covers the source formats under `content/`, how to write
Mexican Spanish for the course, how the compiler (`tools/packc`) turns the
sources into exercises, how to fix each compiler error, and the review
workflow.

`PLAN.md` sections 4.1–4.3 and 7 explain the design. If this guide and the
compiler disagree, the compiler wins. Fix the guide.

---

## Contents

1. [Workflow at a glance](#1-workflow-at-a-glance)
2. [Files](#2-files)
3. [Lessons](#3-lessons)
4. [Sentence markup: linking and targets](#4-sentence-markup-linking-and-targets)
5. [Notes](#5-notes)
6. [The lexicon](#6-the-lexicon)
7. [Stories, phrasebook, confusables](#7-stories-phrasebook-confusables)
8. [Mexican Spanish rules](#8-mexican-spanish-rules)
9. [Register](#9-register)
10. [Respelling](#10-respelling)
11. [How items and distractors are made](#11-how-items-and-distractors-are-made)
12. [Item ids and `ids.lock`](#12-item-ids-and-idslock)
13. [Fit budgets](#13-fit-budgets)
14. [Compiler messages and how to fix them](#14-compiler-messages-and-how-to-fix-them)
15. [Review workflow](#15-review-workflow)
16. [Common mistakes](#16-common-mistakes)

---

## 1. Workflow at a glance

```sh
python3 tools/packc --check                          # validate content/, write nothing
python3 tools/packc --out build/course.pack --dump   # build a pack and build/course.dump.txt
python3 tools/packc --review                         # printable review pages in build/review/
python3 tools/packc --release                        # refuses unreviewed or changed content
```

A normal session:

1. Write or edit the lesson, then add every word it uses to the lexicon.
2. Run `--check` until it reports no errors. It lists every problem in one run.
3. Build with `--dump` and read the lesson in `build/course.dump.txt`: every
   item with its answer options. Check that each exercise makes sense and that
   no wrong option is actually correct (section 11).
4. Run `--review` and read the lesson page as the reviewer will.
5. Commit `content/ids.lock` together with the content (a normal build
   appends to it; `--check` does not).

`test/fixtures/content-mini/` is a small complete example of every source
format. Copy its patterns when in doubt, but do not edit it: tests depend on it.

### Authoring in parallel

When several authors write different units at the same time:

- **Write only your own files.** Your unit's directory (`units/NN-slug/`),
  and for the shared data, a fragment named after your unit instead of an
  edit to the shared file:

  | Shared file | Your fragment |
  |---|---|
  | `confusables.yaml` | `confusables/uNN.yaml` (same format: a list of sets) |
  | `verbs/irregular.yaml` | `verbs/irregular/uNN.yaml` (same format: verbs as keys) |
  | `pronunciation.yaml` | `pronunciation/uNN.yaml` (same format: an `overrides:` mapping) |

  The compiler merges each shared file with its fragments. A confusable set
  name, a verb or an overridden word defined in two files is an error naming
  both places: check the shared file and the other fragments before adding
  one, and leave entries that are already there alone.
- **Lexicon rows may land before their lessons.** Validate your unit with
  `python3 tools/packc --check --allow-missing-examples`: a lemma with no
  example sentence is then a warning, not an error, so other units' rows do
  not block you. Every other error still counts. The flag is for development
  only: a normal `--check`, a build and `--release` (which refuses the flag)
  require an example for every lemma.
- **Review pages of a scratch copy go elsewhere:** `--review` writes to
  `build/review/` only for the default `content/`; with another `--content`
  it writes `review/` next to `--out`, or `build/review-NAME/`
  (`--review-dir` chooses explicitly), so writers do not overwrite each
  other's pages.
- **Never run a build that writes `content/ids.lock`** (a plain
  `python3 tools/packc` or `--out ...`). Use `--check`, which writes nothing.
  The lead builds once at integration, which assigns the new ids in one go.
- Review hashes cover a lesson's own file, its `unit.yaml` and the lexicon
  rows of its new words, never the shared data files or their fragments
  (`confusables`, `verbs/irregular`, `pronunciation`), exactly as before. Edit
  those freely without invalidating anyone's review, but check the verb table
  or respelling on the review page after you change one.

---

## 2. Files

```
content/
  course.yaml                 title, version, locale
  units/00-sonidos/
    unit.yaml                 unit number, titles, goals
    lesson-1.yaml             one lesson: new words, notes, sentences, dialogue
  lexicon/core.tsv            every course word (lemma, gloss, tags)
  lexicon/names.tsv           proper nouns
  lexicon/frequency*.tsv      frequency-deck words outside the lessons (section 6)
  lexicon/phrasebook.tsv      words the phrasebook needs (section 7)
  lexicon/dictionary*.tsv     dictionary-only headwords: lookup, no items (section 6)
  deck/NAME.yaml              example and cloze sentences for deck words (section 7)
  stories/*.yaml              graded readings and dialogues
  phrasebook/*.yaml           one phrasebook category per file
  confusables.yaml            authored distractor sets
  confusables/uNN.yaml        per-unit fragments of the same (section 1)
  pronunciation.yaml          respelling overrides for x words, place names, loans
  pronunciation/uNN.yaml      per-unit fragments of the same
  verbs/irregular.yaml        what the conjugator cannot infer (stem changes, irregulars)
  verbs/irregular/uNN.yaml    per-unit fragments of the same
  ids.lock                    stable item ids: generated, append-only, never edit
```

Encoding is UTF-8 everywhere. YAML notes:

- Quote every Spanish and English string with double quotes: `es: "¿Mande?"`.
  Unquoted text that contains `: `, starts with `¿ ¡ [ { * & ! %` or a quote
  mark, or looks like a number, breaks or changes meaning.
- A title containing a colon must be quoted: `title: "Ser: to be"`.
- Only `true`/`false` are booleans. The Spanish word `no` stays a string.
- Duplicate keys in one mapping are an error.

### course.yaml

```yaml
title: Tinta
version: 1          # integer; bump for each content release
locale: es-MX
```

### units/NN-slug/unit.yaml

The directory number must equal `number`. Lessons are the `lesson-*.yaml`
files in the directory, ordered by their `number` field.

```yaml
number: 1
title: Saludos y cortesía          # Spanish, at most 30 characters
title_en: Greetings and courtesy
goals:                             # English, one line each
  - Greet people and say goodbye at any time of day.
  - Use the subject pronouns, and choose between tú and usted.
```

---

## 3. Lessons

### Fields

```yaml
number: 2                       # lesson number inside the unit; lesson id is u01.l02
title: ¿De dónde eres?          # Spanish, at most 30 characters
title_en: Where are you from?
level: A1                       # A1, A2 (B1, B2 exist for later)
new: [yo, tú, usted, ser, llamarse]     # lemma keys introduced here, in teaching order
conjugate: ["ser:pres", "llamarse:pres.1s"]
notes:
  - kind: grammar               # grammar | culture | pronunciation | usage
    title: Subject pronouns     # English
    text: |
      ...                       # see section 5
sentences:
  - es: "¿{Usted} es de aquí?"
    en: "Are you from here?"
    note: "Usted takes the same verb form as él and ella."   # optional, English
  - es: "¿Cómo te {llamas}?"
    en: "What's your name?"
    order: true                 # optional: also a word-order exercise
  - es: "El {baño} está en el segundo piso."
    en: "The restroom is on the second floor."
    allow: [piso]               # optional: words exempt from the Peninsular lint (piso = floor)
    id: u04-bano-piso           # optional: pins the item ids of this sentence (section 12)
    reviewer_notes: "Is segundo piso how you'd say it?"   # optional: a question for the reviewer
dialogue:
  title: En la escuela          # Spanish
  title_en: At school
  lines:
    - speaker: Maestra          # a label only; not linked, not translated
      es: "Buenos días. Yo soy la maestra Elena."
      en: "Good morning. I'm Elena, your teacher."
```

Do **not** add `reviewed:` yourself. It records the human review (section 15).

`reviewer_notes:` (a string or a list) may go on a lesson, on its `dialogue:`,
on any sentence or dialogue line, and on story and phrasebook files and their
lines and phrases: judgement calls you want the reviewer to confirm (a register
tag, a regional usage, a translation). They appear prominently at the top of
the file's review page and next to the sentence. They are not in the pack and
not part of the review hash, so answering one does not undo a sign-off.

### Size and shape

| Part | Target |
|---|---|
| `new` | 8–12 lemmas, in the order you teach them. Each becomes a recognise and a produce item; a noun with gender `m` or `f` also gets a gender item. A lemma can be introduced in only one lesson. Names cannot be `new`. |
| `notes` | 1–3 notes: a grammar or pronunciation note, plus a culture or usage note where it helps. |
| `sentences` | 15–25. Every new word appears in at least one, ideally two, as the target. |
| word order | 2–4 sentences with `order: true`, each 3–8 words long. |
| `dialogue` | 4–8 lines, two or three named speakers, using the lesson's words in a realistic scene. |
| `conjugate` | Verb tables to drill: `verb:tense` drills all persons (five; three for `imp` and `impneg`), `verb:tense.person` drills one form. Tenses: `pres pret impf fut cond subj imp impneg`. Persons: `1s 2s 3s 1p 3p`. |

### Level targets

| Level | Lesson sentence | Dialogue line | Grammar in sentences |
|---|---|---|---|
| A1 | aim for 10 words or fewer | about 12 words | present tense, `ser`/`estar`, `ir a` + infinitive, formulas |
| A2 | aim for 15 words or fewer | about 18 words | adds preterite, imperfect, object pronouns, commands |

The hard limits are characters, not words (section 13). Use the grammar of the
current and earlier units; an irregular form from a later unit is fine when it
is a fixed formula (`¿Cómo está?`, `Ahorita vengo.`).

### Writing good sentences

- One target per sentence. The target is the word the cloze exercise blanks
  out, so the rest of the sentence and the English must make exactly one
  option correct.
- Natural, everyday Mexican Spanish: what a person in Mexico City, Guadalajara
  or Monterrey would actually say. No textbook sentences that nobody says.
- English translations in natural American English (check, restroom, apartment,
  mom, cell phone), translating the meaning, not word for word:
  `Mis papás` = "my parents", `¿Me da un café?` = "Can I have a coffee?".
- Correct accents and Spanish punctuation: `¿...?`, `¡...!`, a dash `—` before
  a reply inside one sentence (`Gracias. —De nada.`), and typographic quotes
  `“ ”` in stories.
- Prefer concrete, useful sentences a learner can use in Mexico.
- A sentence `note:` is one short English line: a usage hint, not a lecture.

---

## 4. Sentence markup: linking and targets

Every word of every Spanish sentence (lesson sentences, dialogue lines, story
lines, phrases) is linked to a lemma in the lexicon, so the reader can gloss
any word. Function words (`el`, `la`, `de`, `que`, `y`, `me`, `te`, `se`) need
lexicon entries too.

**Spell numbers out** in sentences, titles, questions and options:
`Son veinte pesos`, not `Son 20 pesos`. Digits teach nothing about the Spanish
number and the respelling cannot read them, so the compiler refuses them
(`[digits]`); a sentence that really needs one says `allow: [20]` and the digits
become a number token with no lemma. Notes may use digits where Spanish does
(`calle Juárez 25`, the street `5 de Mayo`).

### Markup

| Markup | Meaning |
|---|---|
| `{casa}` | **target**: emphasised, and in a lesson sentence a cloze item is made from it |
| `{eres\|ser}` | target, linked explicitly to the lemma key `ser` |
| `{eres\|ser:pres.2s}` | target, linked to `ser`, and checked to be that exact form |
| `[fue\|ser]` | link only, no target: use it to disambiguate |
| `[Bueno\|bueno#hello]` | link to a homograph's key |

Rules:

- Punctuation goes outside the braces: `¿{Mande}?`, not `{¿Mande?}`. A marked
  span must start and end with a letter and cannot split a word.
- A marked span with a space is a multi-word lemma: `{Buenos días}, señora.`
  Its words must equal the headword's words (case and the headword's own
  `¿ ? ¡ !` aside): `¿{Qué tal}, Luis?` links to the headword `¿qué tal?`.
- Targets in dialogue, story and phrasebook lines are only shown in bold;
  cloze items come from lesson `sentences` only. The compiler warns
  (`[markup] {target} only makes the word bold here`); use `[word|lemma]` there.
- A verb with attached pronouns (`dígame`, `pásele`) can be linked but cannot
  be a target. Target the verb in another sentence.

Form tags: verb forms `pres.3s`, `pret.1s`, `imp.2s`, `impneg.3p` (tense and
person as above), `inf`, `ger`, `part`, `part.f`, `part.mpl`, `part.fpl`;
noun and adjective forms `pl`, `f`, `mpl`, `fpl`; `apoc` (`buen`); `base`.
There is no `2p`: the compiler rejects vosotros forms.

### How an unmarked word is linked

1. **Multi-word lemmas first, when they stand alone.** If the next words spell
   a multi-word headword (`por favor`, `buenos días`, `de nada`), they become
   one token, longest match first, but only when the phrase stands between
   punctuation or the ends of the sentence: `Gracias. —De nada.`,
   `Un taco, por favor.` Inside a longer clause (`No sé de nada`,
   `Hablo un poco de español`) the words are linked one by one and the
   compiler warns (`[multiword]`): mark `[un poco|un poco]` if you mean the
   expression, or leave it to be linked word by word. The words must be
   separated by single spaces. Multi-word names (`Estados Unidos`) always match.
2. **Exact headword.** Case is ignored, accents are not: `el` and `él`, `papa`
   and `papá`, `tu` and `tú` are different words. A headword's own `¿ ? ¡ !`
   are ignored (`mande` matches `¿mande?`).
3. **Generated forms**: verb forms from the conjugator, plurals, feminines and
   the `forms` column. If the form belongs to two different lemmas, the
   compiler stops with an *ambiguous* error and you mark it: `[fue|ser]` or
   `[fue|ir]`.
4. **Verb plus attached pronouns**: infinitives, gerunds and imperatives with
   `me te se lo la le nos los las les` and pairs (`dámelo`, `pásele`).

**Homographs.** When two lemmas share a headword (`bueno` adjective and
`bueno#hello` interjection), step 2 picks the one that comes first in the
lexicon and warns (`[homograph] 'bueno' is linked to ... by default`). A
headword also beats a generated form of another lemma: `Trabajo en una
oficina` links `trabajo` to the noun, not to `trabajar` (pres.1s), and warns
(`... is also a form of 'trabajar'`). Common pairs: trabajo/trabajar,
cocina/cocinar, cena/cenar, cambio/cambiar, pregunta/preguntar,
ayuda/ayudar, cuenta/contar, comida/comer, como/comer, entre/entrar,
bajo/bajar, para/parar, sal/salir. Collisions among function words are not
reported: `la`, `los` and `las` link to the object pronoun (`lo`, with forms
`f=la; mpl=los; fpl=las`) when a conjugated verb follows directly (`La veo`,
`no las compres`) and to the article everywhere else (`la casa`); mark the
rare exception. Mark every other such word with the lemma you
mean, `[bueno|bueno]`, `[Trabajo|trabajar]` or `¿{Bueno|bueno#hello}?`; the
warning goes away. Because the warning appears as soon as another unit adds
the second lemma, re-run `--check` on your unit after lexicon merges.

**Words that should link only where you mark them.** Some iconic words are
spelled like a very common form of another word: `sale` ("OK!") and `salir`,
`¡aguas!` ("watch out!") and `agua`, `renta` and `rentar`, the noun `ayuda` and
`ayudar`. Give such a row `link: marked` (and a simple key: `sale#ok`,
`aguas#cuidado`). It is a normal course word, with its own gloss, register tag,
items and examples, and it can be a cloze target or a wrong option; but no
unmarked word ever links to it and it causes no homograph warnings, so every
unmarked `sale` keeps linking to `salir`. Mark each use: `¡{Sale|sale#ok}!`,
`¡[Aguas|aguas#cuidado]!`. Then check the grey link lines
on the review page.

**Reflexive verbs** are lemmas with `-se` (`llamarse`). In a sentence the
pronoun and the verb are separate words: in `Me llamo Ana`, `me` links to the
pronoun `me` and `llamo` to `llamarse` (pres.1s).

**Context settles many inflected forms** that belong to two lemmas:

- A reflexive verb's form counts only right after its own pronoun:
  `¿Viste el partido?` is `ver`, `Se viste` is `vestirse`; `Ana está enferma`
  is the adjective, `Ana se enferma` the verb; `Me baño` is `bañarse`, even
  though `baño` is also a noun.
- A participle gives way to an adjective or noun spelled the same:
  `la semana pasada` (`pasado`), `Estoy perdida` (`perdido`), `las comidas`
  (`comida`, not `comer`).
- A noun plural against a verb's tú form (`sales`, `cenas`, `cocinas`) is the
  noun after a determiner (`las sales`, `tus cenas`) and the verb otherwise
  (`¿A qué hora sales?`), with a warning so you confirm.
- What context cannot settle stays an error, e.g. `fue` (`ser` or `ir`): mark it.

A verb and its reflexive twin can both be lemmas (`llamar` and `llamarse`,
`ir` and `irse`, `quedar` and `quedarse`). A form they share links to the
reflexive verb when the word right before it is a reflexive pronoun of the
same person (`me llamo`, `te llamas`, `se llama`, `nos vamos`), and to the
plain verb otherwise (`Voy al mercado`; `Ana me llama`, where `me` is the
object, not the subject). With attached pronouns (`irme`, `llamarte`) the
compiler picks the reflexive verb for `me te se nos` and the plain one
otherwise, and warns so you confirm. When the pronoun stands before an
auxiliary (`Me voy a quedar`, `Te tienes que levantar`, `Me estoy bañando`,
`¿Me la puedo probar?`),
the bare infinitive or gerund links to the reflexive verb and the auxiliary to
its plain verb (`voy` is `ir`); without a pronoun in the clause it is the plain
verb (`Voy a quedar con Ana`). `{quedar|quedarse}` marks it explicitly. Mark
the word when the rules guess wrong (`Me lo [pongo|ponerse]`).

**`hay`** ("there is/are") is a form of `haber`: `content/verbs/irregular.yaml`
gives `haber` an `extra: {hay: pres.3s}` entry, so with a `haber` row in the
lexicon (gloss `to have (auxiliary); there is, there are (hay)`) every `hay`
links to it. The verb table keeps `ha`; `hay` is never drilled as a form. Use
`extra:` the same way for any other form outside the table.

**Names** (`Ana`, `Oaxaca`) need a row in `lexicon/names.tsv`. Speaker labels,
story titles, question texts and answer options are not linked; they are only
checked for characters and Peninsular forms.

---

## 5. Notes

A note is a short explanation shown with the lesson. Kinds: `grammar`,
`culture`, `pronunciation`, `usage`. Titles are English.

### Markup

| Markup | Shows as | Use for |
|---|---|---|
| `_casa_` | Spanish typeface (Times) | every Spanish word or phrase |
| `*strong*` | bold | one key word; inside `_..._` it gives bold Spanish: `_¿Cómo *está* usted?_` |
| `` `KAH-sah` `` | respelling style | pronunciation, in the respelling conventions (section 10) |
| `~vosotros~` | struck through, marked "not Mexican" | Peninsular forms you mention to warn against; exempt from the lint |
| blank line | new paragraph | |
| line starting `- ` | bullet | lists of examples |
| `\_` `\*` `` \` `` `\~` | the literal character | rarely needed |

Lines of one paragraph are joined, so you can wrap long lines. No tables, no
headings, no links.

A note may have `allow: [piso]`, like a sentence, to use a linted word in its
Mexican sense (`_piso_` = floor) in its Spanish examples.

### Style

- **Short**: about 600 characters is typical; stay under 1,200. The hard limit
  is 1,800 characters of text (about three screens).
- **One idea per note.** Split "r and rr" from "z, ce and ci".
- **Rule first, then examples.** One plain sentence stating the point, then
  2–5 examples as bullets, preferably with words from the lesson.
- **Concrete and practical.** Tell the learner what to say and when. Avoid
  linguistics terms; if you need one (seseo), explain it in the same sentence.
- **For English speakers.** Compare with English sounds and habits where it
  helps ("like the tt in American butter").
- Spanish in notes is checked for characters and Peninsular forms but is not
  linked, so a note may mention words that are not in the lexicon.
- Mention a Peninsular form only to say Mexico does not use it, inside `~...~`.

Example:

```yaml
  - kind: pronunciation
    title: r and rr
    text: |
      Spanish has two r sounds, and they tell words apart.

      - A single _r_ between vowels is one quick tap, like the "tt" in American "butter": _pero_ `PEH-roh` (but).
      - _rr_, and _r_ at the start of a word, is a trill: _perro_ `PEH-rroh` (dog).
```

---

## 6. The lexicon

`lexicon/core.tsv` holds every course word. It is **TAB-separated** (not
spaces), with a header row naming the columns; columns can be in any order and
optional ones can be left out. Lines starting with `#` are comments: use them
to group rows. Only put a word in the lexicon when a sentence uses it, and use
every word you add: a lemma without an example sentence fails the build.

| Column | Required | Content |
|---|---|---|
| `lemma` | yes | The headword: infinitive for verbs (`llamarse` for reflexives), masculine singular for nouns and adjectives, the full expression for multi-word lemmas (`por favor`). An expression may keep its `¿?¡!` (`¿qué tal?`, `¿mande?`): linking, cloze targets and cloze options ignore them. |
| `key` | homographs only | A distinct key for a second lemma with the same headword: `bueno#hello`, `papa#pope`. Defaults to the lemma. |
| `pos` | yes | `noun verb adj adv pron det prep conj interj num expr propn` |
| `gender` | nouns | `m`, `f`, `mf` (both: `amigo`/`amiga`, `estudiante`), `mpl`/`fpl` (plural-only: `lentes`). Also allowed on pronouns and determiners; an error on other parts of speech. |
| `gloss` | yes | English. Senses separated by `; `. Verbs start with "to". Put context in parentheses: `you (formal)`, `to be (location, state)`. At most 66 characters; keep it under about 30 so it fits an answer button. |
| `level` | yes | `A1 A2 B1 B2` |
| `register` | | `neutral` (default when empty), `formal`, `informal`, `vulgar` (section 9) |
| `mx` | | `x` for a Mexico-specific word or sense (`jugo`, `camión` = bus, `papa` = potato, `ahorita`) |
| `topic` | | A free tag; distractors from the same topic are preferred. Reuse existing tags: `greetings courtesy people family home food city places things time numbers describing feelings questions grammar language`. |
| `forms` | | Irregular inflections, `;`-separated: `pl=mis`, `f=toda; mpl=todos; fpl=todas`, `apoc=buen`. Tags: `pl f fpl mpl apoc`. Each form given is used on its own (`fpl=cuántas` works without `f=`). `-` says a form does not exist: `pl=-` for nouns with no plural (`fútbol`, `don`), so no plural is generated or offered as a distractor; months have none by default. |
| `alt` | | Accepted non-Mexican synonyms, for typed answers: `zumo` for `jugo`, `ordenador` for `computadora`. Exempt from the lint. |
| `note` | | One short English usage note: `Standard: el trabajo.`, `Mis papás: my parents.` |
| `pron` | | A respelling override for this headword only (section 10). Rarely needed. |
| `freq` | | Frequency rank (integer), used by `frequency.tsv`. |
| `link` | | `marked` for a word spelled like a common form of another word (`sale`, `aguas`): sentences link to it only where marked (section 4). |
| `syn` | | Near-synonyms, `;`-separated lemma keys (`disculpe; con permiso` on `perdón`). The two are never offered as each other's wrong answer; the relation works both ways, so list it on one side. |

What the compiler generates by itself:

- **Plurals** of nouns and adjectives (`limón` → `limones`, `papá` → `papás`,
  `lunes` → `lunes`). Override irregular ones with `pl=`.
- **Feminines** of adjectives and of `mf` nouns ending in `-o`, `-or`, `-ón`,
  `-án`, `-ín`, `-és` (`niño` → `niña`, `doctor` → `doctora`, `inglés` →
  `inglesa`). Override with `f=`.
- **Nothing** for determiners, pronouns and numbers: give their forms
  explicitly (`mi` needs `pl=mis`).
- **All verb forms**, from the conjugator. Regular verbs and spelling changes
  (`busqué`, `conozco`, `sigo`, `construyo`, `leyó`) need nothing. Everything
  else is declared in `content/verbs/irregular.yaml` (its header documents the
  keys): stem changes (`cerrar: {stem: e>ie}`), irregular yo forms, strong
  preterites, prefixed verbs (`mantener: {like: tener}`), written accents
  (`enviar: {accent: true}`). `tools/packc/data/verb_patterns.tsv` is a
  hand-written list of the common verbs the rules alone get wrong, with their
  pattern, plus verb families written as endings (`-tener` covers `mantener`,
  `obtener`, `detener`; `-volver` covers `devolver`). The compiler refuses a
  lexicon verb on that list that `irregular.yaml` does not declare, naming the
  pattern and the entry to add (`[verb] 'devolver' is not regular (irregular,
  like volver) ... add devolver: {like: volver}`). If the verb really is regular
  in the sense you teach, add `verb: {regular: true}`. The list is checked
  against an external verb table: every verb there that the rules get wrong is
  on it. A rare verb outside both could still be conjugated as regular, so
  check the verb table on the review page or in the dump for every new verb,
  and add a missing irregular verb to the list. `python3 -m unittest discover -s
  tools/packc/tests -t tools` re-checks the conjugator after an edit.

Make separate lemmas when the forms carry different meanings or the learner
should meet them separately: `el`, `la`, `los`, `las`; `un`, `una`;
`señor`, `señora`, `señorita`; `nosotros`, `nosotras`.

`lexicon/names.tsv` has the columns `lemma pos gloss level`, `pos` always
`propn`. Names need no example and get no items. Multi-word names
(`Estados Unidos`) work like multi-word lemmas.

### The frequency deck: `lexicon/frequency*.tsv`

Any number of files named `frequency*.tsv` (`frequency-food.tsv`,
`frequency-verbs.tsv`) list frequent words that no lesson introduces, with the
normal columns plus `freq` (frequency rank, 1 = most frequent). Each word gets
recognise, produce (and, for nouns, gender) items in a frequency deck after
the last lesson, in `freq` order; the learner meets them once the unlocked
lessons have no new words left for the day. Each still needs an example
sentence: write it in a deck file (`deck/*.yaml`, section 7), where marking
the word as a target also makes a cloze item.

### Dictionary-only headwords: `lexicon/dictionary*.tsv`

Files named `dictionary*.tsv` hold headwords that are only for the dictionary:
searchable by Spanish, by English keyword and by inflected form, with a
respelling and, for verbs, a verb table. They make no items, need no example
sentence, are never offered as a wrong option, and may leave `level` empty.
Columns: `lemma key pos gender gloss level register mx forms note pron` (only
these). A sentence links to a dictionary headword only where it is marked
(`[abeja|abeja]`); unmarked words never link to one, so adding dictionary rows
never changes the links of existing lessons. A dictionary verb on the
irregular-verb list that `verbs/irregular.yaml` does not declare yet gets no
verb table, without an error: the build prints a `note [dictionary]` listing
them so they can be declared later. Keys must not repeat a course word's key.

The dictionary may define a word the Peninsular lint refuses (`coger`,
`chaqueta`, `ordenador`, `móvil`, `zumo`, `patata`, `piso`, `vale`, `gafas`,
`aparcar`), because learners will look exactly those up, provided the row has
a `note` that warns and gives the Mexican word: `Vulgar in Mexico; say tomar
or agarrar.`, `Jacket in Spain; in Mexico say chamarra (chaqueta is vulgar
slang here).`, `In Mexico: la computadora.` Tag the register too (`coger` is
`vulgar`). Without a note the row is refused, and the lint still applies to
every sentence, note and course row.

---

## 7. Stories, phrasebook, confusables

### stories/NAME.yaml

```yaml
title: Ana en la Ciudad de México
title_en: Ana in Mexico City
level: A1
kind: reading              # reading | dialogue
lesson: u01.l03            # optional: the lesson after which it unlocks
lines:
  - es: "Ana es de Chicago, pero vive en México."
    en: "Ana is from Chicago, but she lives in Mexico."
  - es: "Todos los días, Ana come en una fonda."
    en: "Every day, Ana eats at a fonda, a small family restaurant."
    paragraph: true        # starts a new paragraph
  # a dialogue story adds `speaker:` to each line
questions:
  - q: "¿De dónde es Ana?"
    options: ["De Chicago", "De Oaxaca", "De la calle Xola"]   # 2 to 4
    answer: 0              # 0-based index of the right option
  - q: "What does the man say to Ana?"
    lang: en               # es (default) or en
    options: ["Enjoy your meal!", "Good night!", "What's up?"]
    answer: 0
```

Stories use only words the learner has met by their lesson, plus a few new
ones the reader can gloss. 6–12 lines at A1. Questions test understanding, not
memory of a single word; options at most 40 characters.

### phrasebook/NAME.yaml

One category per file; `order` sorts the categories.

```yaml
title: En la taquería
title_en: At the taco stand
order: 2
phrases:
  - es: "¿Me da tres tacos [al pastor|al pastor], por favor?"
    en: "Can I have three tacos al pastor, please?"
    note: "¿Me da...? is the everyday way to order."   # optional
    pron: ""        # optional respelling override for the whole phrase
    id: taq-pedir   # optional: pins the item id (section 12)
```

Phrases are what a traveller needs to say or understand, ready to use. Each is
linked like a sentence and respelled as a whole. Spanish at most 90 characters.
Spell numbers out (`veinte pesos`), as in every sentence (section 4). A phrase
may carry `note:`, `pron:` (its whole respelling), `id:` and `reviewer_notes:`.

Plan about ten category files of about thirty phrases. Words that only the
phrasebook needs go in `lexicon/phrasebook.tsv` (normal columns); a phrase that
uses a word counts as its example. Phrase items are practised only from the
phrasebook ("practise this category"), never in the daily session.

### deck/NAME.yaml

Example sentences for frequency-deck words, in no lesson. Same sentence
format as lessons (section 3: `es`, `en`, `note`, `order`, `allow`, `id`,
`reviewer_notes`, `{targets}`).

```yaml
title: Comida y bebida        # optional, shown on the review page
sentences:
  - es: "¿Me da una {manzana}, por favor?"
    en: "Could I have an apple, please?"
  - es: "Las {manzanas} están muy ricas."
    en: "The apples are delicious."
    order: true
reviewer_notes: "Is 'ricas' natural for fruit?"   # optional
```

A `{target}` makes a cloze item, placed after the deck words' flashcards in
the frequency order of the word it practises, and gated by that word's
recognise item. Sentences may use any course or deck word. Deck files are
reviewed like lessons (`--mark-reviewed NAME`); the review hash covers the file
and the lexicon rows of the deck words it practises. Name deck files so they
do not clash with story or phrasebook file names.

### confusables.yaml

Authored distractor sets: members are always offered as wrong options for
each other first, in vocabulary and cloze exercises. Use them for pairs a
learner really mixes up.

```yaml
- name: papa / papá
  lemmas: [papa, papá]     # 2 to 6 lemma keys
  note: The written accent changes the word. Papa is a potato; papá is dad.
```

A cloze on a confusable member shows the partner too: in the same form when
it is the same part of speech, in its dictionary form otherwise (`Tengo mucho
[calor]` offers `caliente`; `pero` / `perro`), so the sentence must rule the
partner out. Vocabulary items offer members against each other whatever their
part of speech.
Do not put true synonyms in a set (`perdón`, `disculpe`, `con permiso`): in
many sentences either would be correct.

---

## 8. Mexican Spanish rules

Tinta teaches the Spanish of Mexico. The compiler fails the build on the
Peninsular forms marked **lint**; the others are style rules for writers and
the reviewer.

### Vocabulary

| Do not write | Write (Mexico) | |
|---|---|---|
| vosotros, vosotras, os, vuestro | ustedes, les/los/se, su / de ustedes | **lint** |
| -áis / -éis verb forms (habláis, coméis) | ustedes forms (hablan, comen) | **lint** |
| ordenador | computadora | **lint** |
| móvil | celular | **lint** |
| zumo | jugo | **lint** |
| patata | papa | **lint** |
| piso (apartment) | departamento | **lint**; `allow: [piso]` for "floor" |
| vale (OK) | sale, va, está bien, ok | **lint**; `allow: [vale]` for "vale la pena" or "it costs" |
| gafas | lentes | **lint** |
| aparcar | estacionar | **lint** |
| coger | tomar, agarrar (coger is vulgar in Mexico) | **lint** |
| chaqueta | chamarra (chaqueta is vulgar slang) | **lint** |
| guay | padre, chido (informal) | **lint** |
| melocotón | durazno | **lint** |
| judías | frijoles | **lint** |
| conducir | manejar | style |
| autobús | camión (city bus), autobús is fine for long-distance coaches | style |
| billete (ticket) | boleto (a billete is a banknote) | style |
| camarero | mesero | style |
| piscina | alberca | style |
| cacahuete | cacahuate | style |
| tomate (red) | jitomate (in central Mexico, tomate is the green tomatillo) | style |
| pajita | popote | style |
| acera | banqueta | style |
| enfadarse | enojarse | style |
| coche | carro (coche is also common in Mexico City) | style |
| mis padres | mis papás (padres is fine but formal) | style |

### Grammar and usage

- **No vosotros.** Five persons in every verb table: `yo, tú, él/ella/usted,
  nosotros, ellos/ellas/ustedes`. Plural "you" is always `ustedes`.
- **Tú and usted.** Usted with strangers, older people, customers, service
  staff and officials; tú with friends, family, children and people your age.
  Keep each conversation consistent. Service dialogues use usted.
- **Preterite for completed past**, where Spain often uses the present perfect:
  `Hoy comí tacos`, not `Hoy he comido tacos`. `¿Ya comiste?`.
- **Mexican courtesy**: `¿mande?`, `con permiso`, `provecho`, `¿me permite?`,
  `¿me da...?` for ordering, `¿bueno?` on the phone, softening diminutives
  (`ahorita`, `un momentito`, `un cafecito`).
- **Address**: `señor`, `señora`, `joven`, `maestro/maestra` for teachers;
  `güero/güera` and other nicknames are informal.
- **Numbers and money**: prices in pesos, spelled out (`Son cincuenta pesos.`;
  section 4). The 24-hour clock is for timetables; speech uses `de la mañana`,
  `de la tarde`.
- **Spelling**: `México`, `mexicano` (never `Méjico`); accents on question
  words (`¿Qué?`, `¿Dónde?`); `ü` where needed (`pingüino`).

### Characters

Only Latin-1 letters and punctuation plus `– — ‘ ’ “ ” • € …` can be drawn.
No emoji, arrows, ticks, `≠`, macrons (`ā`) or other symbols. Straight quotes
`"` and `'` are fine; prefer `“ ”` and `’` in running text.

---

## 9. Register

Every lemma has a register; a sentence takes the strongest register of its
words.

| Register | Meaning | Examples |
|---|---|---|
| `formal` | written or ceremonious; odd among friends | `estimado`, `atentamente` |
| `neutral` (default) | fine with anyone | most words, `ahorita`, `¿mande?`, `provecho` |
| `informal` | colloquial: fine with friends and people your age, wrong with a boss, an official or an elder you do not know | `órale`, `¿qué onda?`, `chido`, `padre` (= great), `chamba`, `lana` (= money), `neta`, `cuate`, `no manches`, `güey` |
| `vulgar` | offensive or taboo in polite company; hidden from learners unless they enable it in settings | `pinche`, `chingar` and its family, `pendejo`, `cabrón`, `coger`, `verga`, `culero` |

- The compiler requires at least `informal` or `vulgar` for the words in its
  list (`MIN_REGISTER` in `tools/packc/validate.py`). Tag any other colloquial
  or rude word the same way even if the list misses it, and add a lexicon
  `note` with the neutral word (`Standard: el trabajo.`).
- A word that is neutral in one sense and colloquial in another needs two
  lemmas with keys: `padre` (father, neutral) and `padre#cool` (adj,
  informal).
- When unsure between informal and vulgar, choose vulgar: hiding a word is
  safer than teaching it to say at work.
- Mark informal usage in sentence notes too: `note: "Informal: only with
  friends."`

---

## 10. Respelling

Tinta has no audio, so every lemma and phrase carries an English-based
respelling, generated by `tools/packc/respell.py`. Its docstring is the full
table; the essentials:

- Plain ASCII; syllables joined by `-`, words by a space; the stressed
  syllable in CAPITALS: `chamba` → `CHAHM-bah`, `gracias` → `GRAH-syahs`.
- Vowels: `a e i o u` = `ah eh ee oh oo`; `ai` = `eye`, `ei` = `ay`, `oi` = `oy`,
  `au` = `ow`; `ia ie io` = `yah yeh yoh`; `ua ue ui` = `wah weh wee`.
- Mexican consonants: seseo (`z`, `ce`, `ci` = `s`: `cerveza` →
  `sehr-BEH-sah`), yeísmo (`ll` and `y` = `y`: `calle` → `KAH-yeh`), `j` and
  soft `g` = `h` (`gente` → `HEHN-teh`), silent `h`, `ñ` = `ny`, `b`/`v` = `b`,
  trilled `rr` (`perro` → `PEH-rroh`), `tl` kept together.
- `x` is `ks` by rule (`taxi` → `TAHK-see`). The other readings (`México`
  `MEH-hee-koh`, `Xochimilco` `soh-chee-MEEL-koh`, `Xola` `SHOH-lah`), place
  names and English loans go in `content/pronunciation.yaml`. An entry there applies to that exact word form
  everywhere, phrases included, so inflected forms are listed separately
  (`mexicano`, `mexicana`).
- Use the lexicon `pron` column (or a phrase's `pron:`) only for a one-off
  reading of a single lemma or phrase that the rules and
  `pronunciation.yaml` cannot express. It replaces the respeller entirely.

Respellings written by hand in notes (`` `KAH-sah` ``) must follow the same
conventions; copy them from the dump or review page so they match the cards.

---

## 11. How items and distractors are made

### Items per lesson

| Item | Made from | Exercise |
|---|---|---|
| recognise | each `new` lemma | Spanish → English flashcard or multiple choice |
| produce | each `new` lemma (unlocks after recognise) | English → Spanish |
| gender | each `new` noun with gender `m` or `f` | el / la |
| conjugation | each `conjugate` entry | pick the verb form |
| cloze | each `{target}` in lesson `sentences` | fill the gap from four options |
| word order | each sentence with `order: true` | arrange the word tiles |
| phrase | each phrasebook phrase | recognise the phrase |

### Distractors (wrong options)

Multiple-choice items need at least three distractors, or the build fails
(one is enough for a closed-class cloze target, see below).

- **Vocabulary items** draw other lemmas from the same part-of-speech group:
  nouns; verbs; adjectives; adverbs; interjections and expressions together;
  pronouns, determiners, prepositions and conjunctions together; numbers.
  The level must be within one step (A1 with A2), and the **English senses
  must not overlap**. A sense is a `;`-separated piece of the gloss with
  parentheses removed and a leading "to", "a", "an" or "the" dropped: `you
  (informal)` and `you (formal)` share the sense "you", so `tú` and `usted` are
  never offered against each other in a vocabulary item. Lemmas linked by the
  `syn` column are never offered against each other either.
  Same-topic lemmas come first, then confusables, lemmas taught nearby, and
  similar levels.
- **Cloze items** offer other forms in the same slot: for a verb, other
  persons and tenses of the same verb; for anything else, lemmas from the
  vocabulary pool **in the gender and number the sentence shows**, so an
  option is never ruled out by agreement alone: `una [casa]` offers feminine
  nouns, `dos [tacos]` masculine plurals, `[chidas]` feminine plural adjectives
  (`grandes` too, since it has no feminine form). An adjective that does not
  show its gender (`mayor`, `joven`) agrees with the nearest noun, article or
  pronoun before it: in `mi hermana {mayor}` and `Las pirámides son
  {increíbles}` the options are feminine. A noun of both genders (`policía`,
  `estudiante`) follows its article: `la [policía]` offers feminine nouns. Headword punctuation is
  dropped (`mande`, not `¿mande?`). Options longer than 28 characters are
  skipped.
- **"You" forms.** A verb target in the tú form never gets the usted form of
  the same verb as an option (or the reverse) unless the Spanish decides it:
  `¿De dónde [eres]?` ("Where are you from?") does not offer `es`, but
  `Tú [eres]...`, `¿Cómo te [llamas]?` or `¿Usted [es]...?` may. Markers:
  `tú te ti tu tus contigo` and `usted su sus le señor señora señorita don
  doña` or an usted command.
- **Verb forms only in tenses already taught.** A verb cloze or conjugation
  item offers the same verb's forms in the target's own tense and in tenses
  the lessons have taught by then: the present from the start, commands from
  Unit 5, the preterite from Unit 9, the imperfect from Unit 10 (deck items
  count as after all lessons). Future, conditional and non-command subjunctive
  forms appear only when the target itself is in that tense, so "What do you
  recommend?" never offers `recomendaría`. Early "you" sentences may then have
  only two options (`¿[Habla] inglés?`: `hablo`, `hablamos`); that is allowed.
- **Infinitives and gerunds** get other verbs in the same bare shape:
  `Me voy a [quedar]` offers `bañar`, not `bañarse`.
- **Function words** get options of the same kind only: possessives against
  possessives (`[Nuestro] departamento` offers `mi su tu`), demonstratives,
  articles, subject pronouns, object pronouns, question words; prepositions
  against prepositions. Determiners agree with the noun after them. These are
  closed classes, so one or two options are enough; the exercise then shows
  two or three choices. Demonstratives: `[este]` offers `ese` and `aquel`, but
  `[ese]` and `[aquel]` offer only `este`, since both translate as "that" and
  Mexican usage lets `ese` cover "over there".
- **Options never repeat or overlap.** Within one item no two options look
  the same or share a sense (`trabajo` "job; work" and `chamba` "job; work" are
  never offered together), and vulgar words are only offered against vulgar
  targets. Words the learner has already met (introduced in, or used by, the
  same or an earlier lesson) come first; a later word is offered only when
  fewer than three met words fit.
- **Conjugation items** offer other forms of the same verb.
- **Phrase items** offer other phrases, same category first.

What this means for writers:

- **Introduce words in groups.** A lone adverb or the first verb of the course
  has nothing to be confused with. Teach several of the same kind together.
- **Glosses decide what counts as "different".** Near-synonyms are never
  offered as wrong answers when they share a sense (`perdón`, `disculpe` and
  `con permiso` all include "excuse me") or are linked with `syn`. Give words
  that should be told apart distinct senses.
- **Feminine and plural targets need company.** A cloze on `mexicana` needs
  three other adjectives with a feminine form (`grande` has none); a cloze on
  a plural-only noun has few partners.
- **Make the right answer the only right answer.** Read every cloze in the
  dump with its options. Cloze exercises are designed to be shown with the
  sentence's English translation, which rules out options that are
  grammatical but mean something else (`Buenas tardes` for "Good morning",
  `Usted` for "He"). Still prefer sentences where the Spanish alone decides,
  and never use one where a distractor fits the translation too
  (`¡___! ¡Qué bonito!` "Wow! How pretty!" accepts `¡Ay!` as well as
  `¡Órale!`).
- **Agreement narrows the pool.** A cloze on a feminine noun needs three other
  feminine nouns of a similar level; a plural-only noun (`lentes`) needs
  plural-only or plural partners. Articles count too: after `el`, `un`, `al`
  or `del`, and for a feminine noun that takes `el` (`el agua`, `el hambre`),
  the options are masculine nouns or other nouns like `agua`; after `la` or
  `una`, nouns like `agua` are never offered (`una agua` gives itself away).

### Reading the dump

`build/course.dump.txt` lists every item in teaching order:

```
  33 uid 34    cloze       Es una [casa] muy bonita.  (token 2) after #0
                        options: mesa | llave | cocina | baño | departamento | agua | café | libro
  39 uid 40    cloze       Tacos de [papa], por favor.  (token 2) after #3
                        options: *papá | agua | café | limón | plátano | taco | chile | jugo
```

`[...]` is the gap, `options` are the candidates best first (the device picks
three), `*` marks confusable partners that are always shown, `after #N` is the
item that must be learned first. The lexicon section shows each lemma's
respelling, forms, first examples and full verb table.

---

## 12. Item ids and `ids.lock`

Learner progress is stored per item id, so ids must never change meaning.
`content/ids.lock` maps each item key to a numeric id. A normal build appends
new keys; `--check` never writes it. It is append-only and checksummed: never
edit, reorder or delete a line (the build fails if you do). Commit it with the
content. `--release` refuses to build while items lack an id.

Item keys, and what changes them:

| Item | Key | Changes when |
|---|---|---|
| recognise, produce | `vocab:KEY:recognise` | the lemma key changes |
| gender | `gender:KEY` | the lemma key changes |
| conjugation | `conj:KEY:pres.1s` | the verb key or the form changes |
| cloze | `cloze:SENTENCE:TARGET` (`cloze:la mesa es nueva:mesa`; `:2` for a second identical target) | the Spanish sentence or the target word changes |
| word order | `order:SENTENCE` | the Spanish sentence changes |
| phrase | `phrase:SENTENCE` | the Spanish phrase changes |
| any of these, sentence with `id: ID` | `SENTENCE` is `id:ID` | the `id` or the target word changes |

`SENTENCE` is the folded Spanish text (lower case, no accents or punctuation),
so keys never depend on where a sentence stands: inserting, deleting,
reordering sentences or moving them between lessons or phrasebook files keeps
every id. Fixing a gloss, a translation or a note keeps every id too.
Rewording the Spanish of a sentence without an `id` retires its items (their
progress is dropped) and makes new ones; that is right for a real rewrite. To
reword a shipped sentence and keep its progress, it must already carry an
`id:` (unique across the course) — give one to any sentence you expect to edit
after release. The same sentence text used twice with the same target is an
error; give one of them an `id`. Uids start at 1, and 0xFFFFFFFF is reserved
(the progress store uses it for damaged records).

---

## 13. Fit budgets

Character limits stand in for font measurements; the build fails above them.

| What | Limit |
|---|---|
| headword (lemma) | 24 |
| gloss | 66 (aim for 30) |
| respelling | 40 |
| lesson sentence, Spanish | 132 |
| lesson sentence, English | 172 |
| dialogue and story line, each language | 300 |
| phrase, Spanish | 90 |
| any single word | 22 |
| lesson and unit title | 30 |
| note text | 1,800 (aim for 600, stay under 1,200) |
| story question | 132 |
| story answer option | 40 |
| cloze option (skipped, not an error) | 28 |
| word-order sentence | 3 to 8 words (a multi-word lemma counts as one) |

---

## 14. Compiler messages and how to fix them

Messages read `file:line: what: error [code]: text`. The build reports
everything it finds in one run.

| Message | Fix |
|---|---|
| `[link] unlinked word 'X': add it to the lexicon or mark it [X\|lemma]` | Check the spelling and accents first. Then add the lemma to the lexicon, or a name to `names.tsv`. If `X` is a form the compiler does not generate (an irregular plural, a contraction), add it to the lemma's `forms` column. A verb form that is missing or wrong (`quiero`, `tuve`) means the verb needs an entry in `content/verbs/irregular.yaml`. |
| `[link] ambiguous: 'X' is a form of A, B` | Mark which one: `[fue\|ser]`. |
| `[link] no multi-word lemma 'X'; write [X\|key]` | A marked span with a space must spell a multi-word headword exactly. Fix the span or add the lemma. |
| `[link] unknown lemma 'K' in markup for 'X'` | The key after `\|` is not in the lexicon. Check homograph keys (`bueno#hello`). |
| `[form] 'X' is not K pres.3s (expected 'Y')` | The surface does not match the form tag: fix the tag or the spelling. |
| `[form] 'X' is not a known form of 'K'` | Fix the spelling, or add the form to the `forms` column. |
| `[markup] marked text 'X' must start and end with a letter` | Move punctuation outside the braces: `¿{Mande}?`. |
| `[markup] marked text 'X' splits a word` | Mark the whole word. |
| `[markup] unclosed '_' in note` (or `*`, `` ` ``, `~`) | Close the markup, or escape a literal character with `\`. |
| `[example] lemma 'K' has no example sentence` | Use the word in a sentence, or delete the row. Also appears for a verb whose forms are all unlinked. While another unit's lessons are still missing, check with `--allow-missing-examples` (section 1). |
| `[duplicate] verb 'K' is also defined at ...` / `confusable set ...` / `the respelling of ...` | The entry exists in the shared file or another unit's fragment: keep one. |
| `[distractors] item 'K' has N distractor(s)` | Add lemmas of the same part-of-speech group and level, give near-synonyms distinct senses only if they should be told apart, or change the target (section 11). |
| `[peninsular] 'X' is Peninsular: ...` | Use the Mexican word. If the word is right in its Mexican sense (`piso` = floor), add `allow: [piso]` to the sentence. In a note, put a form you warn against in `~...~`. |
| `[peninsular] 'X' is a vosotros verb form` / `form 'pres.2p' is a vosotros form` | Use the ustedes form. |
| `[charset] character(s) not in the font charset: ...` | Replace the character (section 8): no emoji, arrows, macrons or symbols. |
| `[register] 'X' must be tagged register informal or stronger` | Set the `register` column. |
| `[fit] ... is N characters; the B budget is M` | Shorten it (section 13). |
| `[order] word-order sentence has N words; use 3 to 8` | Pick another sentence, or drop `order: true`. |
| `[cloze] target 'X' has attached pronouns` | Do not target a verb with attached pronouns; target it in another sentence. |
| `[complete] noun 'X' has no gender` / `lemma 'X' has no valid level` / `has no gloss` | Fill the column. |
| `[schema] gender is only for nouns, pronouns and determiners` | Clear the gender column. |
| `[schema] unknown field 'X' (allowed: ...)` | A misspelled or unsupported field name. |
| `[duplicate] lemma key 'K' already defined` | Give the second homograph its own key: `papa#pope`. |
| `[duplicate] lemma 'K' is already introduced in u01.l02` | A word is `new` in one lesson only. |
| `[link] new lemma 'K' is not in the lexicon` | Add the row or fix the key. |
| `[schema] proper noun 'K' cannot be a new lesson word` | Names are not taught as vocabulary. |
| `[link] conjugate 'S': 'K' is not a verb in the lexicon` / `unknown tense` | Use `verb:tense` or `verb:tense.person` with the tenses and persons of section 3. |
| `[link] story lesson 'X' does not exist (use uNN.lNN)` | Fix the `lesson:` reference. |
| `[schema] a question needs 2 to 4 options` / `answer N is not an option index (0-based)` | Fix the question. |
| `[yaml] ...` | Usually an unquoted string with `: ` or a leading `¿`/`¡`. Quote it. |
| `[ids] checksum mismatch` / `out of sequence` | Someone edited `ids.lock`. Restore it from version control. |
| `[review] N of M reviewable files are not reviewed` (warning) | Expected until review; `--release` turns it into an error. Record a finished review with `--mark-reviewed` (section 15). |
| `[homograph] 'X' is linked to 'A' by default but is also the headword of 'B'` (warning) | Mark the word with the lemma you mean: `[X\|A]` or `[X\|B]`. |
| `[multiword] 'X' is inside a longer clause, so it is linked word by word` (warning) | If it is the expression, mark `[X\|key]`; if not, make sure each word has a lemma. |
| `[markup] {target} only makes the word bold here` (warning) | Targets only make clozes in lesson `sentences`; use `[word\|lemma]` in dialogue, story and phrasebook lines. |
| `[digits] spell out the number '20' in Spanish` | Write `veinte`. Digits teach nothing about the Spanish number, and the respelling cannot read them. Exempt one deliberately with `allow: [20]`. |
| `[verb] 'pensar' is not regular (e>ie) but content/verbs/irregular.yaml has no entry` | Add the suggested entry to `content/verbs/irregular.yaml`; if the verb is regular in the sense taught, add `pensar: {regular: true}`. |
| `[duplicate] item '...' generated twice` | The same sentence text with the same target appears twice: reword one or give it an `id:`. |
| `[link] synonym 'K' is not in the lexicon` | Fix the key in the `syn` column. |

---

## 15. Review workflow

`--review` also writes a **How to review Tinta** page (linked from the index)
for a reviewer who is a native speaker but not technical: the kinds of page,
how to mark corrections on paper, and what the tags mean. Each
`lexicon/dictionary*.tsv` file gets a page too, listing first the entries
that need a native speaker's judgement (Mexico flag, register tag, note,
hand-written respelling). Dictionary pages are **not required for release**;
a finished one can still be recorded with `--mark-reviewed dictionary-a-c --by
"Name"`, which writes a `# reviewed:` comment line at the top of the TSV.

Nothing ships unreviewed. The reviewer needs neither the repository nor a
device.

1. The author runs `python3 tools/packc --review` and sends the pages in
   `build/review/` (open `index.html`; each page prints on paper). There is
   one page per lesson, story and phrasebook category. A lesson page shows the
   notes, the new words with tags, respelling, forms and usage notes, the verb
   tables, every sentence with its translation (targets in bold, with grey
   lines showing how each word is linked), and the dialogue.
2. The reviewer checks that the Spanish is natural Mexican Spanish, that the
   translations, glosses, register tags and respellings are right, and that
   the linked words make sense, and writes corrections in the right-hand
   column.
3. The author makes the corrections and runs `--review` again. Each page shows
   a **content hash**. It covers the lesson file (without `reviewed:`), its
   `unit.yaml`, and the lexicon rows of the lesson's new words; for a story or
   phrasebook page, the file itself.
4. The reviewer checks the corrected page (the changes only). When it is
   right, the author records the review:

   ```sh
   python3 tools/packc --mark-reviewed u01.l02 --by "Reviewer's name"
   python3 tools/packc --mark-reviewed ana-en-mexico taqueria --by "..."   # stories, phrasebook files
   python3 tools/packc --mark-reviewed all --by "..."                      # everything at once
   ```

   It writes `reviewed: {by: "...", date: 2026-10-12, hash: '3f9a1c0b2d4e'}`
   into each file with the hash of the content **as it is now**, i.e. the
   corrected text. Never copy a hash from the first printed page: it is the
   hash of the text before the corrections, so it would never match. The
   command refuses to run while the content has errors. Lessons are named
   `uNN.lNN`; stories and phrasebook categories by file name.
5. Any later edit changes the hash. Development builds then warn, and
   `--release` refuses to build until the file is reviewed again. Editing a
   lexicon row of a lesson's new word, or the unit's `unit.yaml`, also
   invalidates the lesson's review. A lesson's dialogue is reviewed with its
   lesson.

---

## 16. Common mistakes

- A word used in a sentence but missing from the lexicon, or a lexicon row no
  sentence uses. Run `--check` after every edit.
- Punctuation inside target braces: `{¿Mande?}`. Write `¿{Mande}?`.
- Punctuation in headwords (`¿mande?`, `¿qué tal?`): it breaks multi-word
  targets and appears inside cloze options (`¿¿mande??`). Write `mande`,
  `qué tal`.
- Two targets in one sentence, or a target whose gap several options fill
  equally well. Read the cloze options in the dump.
- Targeting a subject pronoun where the verb does not decide it: `{Él} es
  Luis` also accepts `Usted es Luis`; only the translation rules it out.
- Relying on the silent homograph default instead of marking `[x|key#sense]`.
- A multi-word lemma that captures text you did not mean (`de nada` inside
  `no sabe de nada`). Mark the words separately with `[de|de]`.
- Glosses that are too long for an answer button, or that accidentally share
  a sense with an unrelated word (both containing "well").
- Peninsular words, `vosotros` outside `~...~` in notes, `coger`, `vale` for
  "OK", present perfect for completed past.
- Forgetting `register: informal` on slang, or the `mx` flag and `alt`
  synonym on Mexican words.
- Writing respellings in notes that differ from the generated ones.
- Editing `ids.lock`, or adding `reviewed:` without a review.
- Inserting sentences in the middle of a shipped lesson (shifts cloze ids).
- Unquoted YAML strings with colons.

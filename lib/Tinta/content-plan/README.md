# Course plan: Units 2–12

The vocabulary and grammar plan for Units 2–12 of Tinta, so that several
writers can draft lessons in parallel. It follows PLAN.md §4.1–4.3 and §7.4
and `docs/content-style.md`. The plan does not contain lessons. It decides
what each lesson teaches, in what order, and with which words.

| Path | What |
|---|---|
| `content-plan/units/NN-slug.yaml` | One plan per unit: titles, goals, lessons, new words, verb drills, dialogue brief, notes, cautions |
| `content-plan/check_plan.py` | Checks the plan against the lexicon, and checks the vocabulary order of written lessons |
| `content/lexicon/u02.tsv` … `u12.tsv` | Lexicon rows for every word the plan introduces, plus supporting words |
| `content/verbs/irregular/plan.yaml` | The three verbs the conjugator needed (acordar, temblar, sonar) |

No pronunciation overrides were needed: every new headword and name is
respelled correctly by rule. The full list was checked in a scratch build.

## For lesson writers

1. Read your unit's plan file and `docs/content-style.md`.
2. Copy each lesson's `new` and `conjugate` lists into `lesson-N.yaml`
   verbatim, in the same order. Write the titles exactly as planned.
3. Write sentences and the dialogue from the brief. Use only:
   - words introduced in this lesson or an earlier one (Units 0–1 included);
   - words that no lesson introduces. These are the `supporting` rows of every
     unit file and the core function words (el, de, que, me, se, muy...).
4. Read the `cautions` for the lesson; most of them are linker traps.
5. Run both checks until they are clean (except `[example]` warnings for other
   units):

   ```sh
   python3 tools/packc --check --allow-missing-examples
   python3 content-plan/check_plan.py u05        # your unit
   ```

`check_plan.py` reports any word that a lesson uses before the lesson that
introduces it. A word counts as introduced where a written lesson's `new` puts
it, or else where the plan puts it.

**Adding words.** If a sentence plainly needs a word that is not in the
lexicon, add a row to your unit's TSV, in the `supporting` section, and add its
key to the plan's `supporting` list. Do not move a planned word to an earlier
lesson, and do not introduce (`new`) a word that is not in your lesson's plan;
tell the lead instead. Names go in your TSV as `propn` rows.

**Every row needs an example sentence by integration.** That includes
supporting rows (numbers 16–29, months, `desayuno`, `cena`...). Rows still
unused when the course is integrated should be deleted.

**Conjugation drills are global.** An item `conj:verb:pres.1s` can exist only
once in the course. The plan's `conjugate` lists never repeat one. Do not add
`ser:pres` or `tener:pres` (already drilled) to any lesson.

## Plan file format

```yaml
unit: 5
dir: units/05-ciudad-transporte       # where the unit's lessons go
title: "La ciudad y el transporte"    # unit.yaml title (<= 30 chars)
title_en: "City and transport"
level: A1
goals: ["...", "..."]                 # unit.yaml goals, English
grammar_scope: "..."                  # what grammar sentences may use, and what not yet
lexicon: content/lexicon/u05.tsv
supporting: [hasta, desde, ...]       # rows in the unit TSV that no lesson introduces
names: [Beto, Zócalo, ...]            # propn rows in the unit TSV
lessons:
  - id: u05.l03
    number: 3
    title: "Siga derecho"             # lesson title, Spanish (<= 30 chars)
    title_en: "Keep going straight"
    level: A1
    grammar:
      focus: "..."                    # the single grammar point
      can_do: "..."                   # what the learner can do afterwards
    new: [perdido, ayudar, ...]       # lemma keys in teaching order: copy verbatim
    rows_in_core: []                  # members of new whose row is in core.tsv
    conjugate: ["seguir:imp", ...]    # copy verbatim
    dialogue:
      title: "Estoy perdido"
      title_en: "I'm lost"
      where: "..."
      who: "..."
      address: "usted both ways: strangers."   # tú or usted, and why
      happens: "..."                  # the scene; not the script
    notes:                            # 2-3 notes: kind, title, what to cover
      - {kind: grammar, title: "...", covers: "..."}
    confusables: ["derecha / derecho"]   # sets to write in confusables/uNN.yaml
    cautions: ["...", "..."]          # traps for the writer
```

Confusable sets are proposals. The writer adds them to `confusables/uNN.yaml`
with the members' keys (`ser / estar` means `lemmas: [ser, estar]`).

## Grammar progression

| Lesson | Title | Grammar |
|---|---|---|
| u02.l01 | ¿A qué te dedicas? | noun gender; el, la, un, una; jobs without article |
| u02.l02 | Uno, dos, tres | plurals; los, las, unos, unas; numbers 0–10 |
| u02.l03 | ¿Cómo estás? | estar for place and state; ser vs estar |
| u02.l04 | ¿Cuál es tu número? | numbers 11–30; ¿cuál? |
| u03.l01 | ¿Qué quieres? | querer + noun or infinitive |
| u03.l02 | Me encanta el picante | gustar, encantar; me/te/le/nos/les |
| u03.l03 | ¿Me trae la cuenta? | polite usted requests: ¿me da?, ¿me trae?, ¿me puede...? |
| u03.l04 | ¿Cuánto es? | numbers 31–100; prices; costar, pagar |
| u04.l01 | Mi familia | tener; age with tener + años |
| u04.l02 | Mi casa es su casa | possessives mi, tu, su, nuestro; de + person |
| u04.l03 | ¿Hay agua caliente? | hay vs estar |
| u04.l04 | ¿Cómo es? | adjective agreement and position |
| u05.l01 | ¿A dónde vas? | ir + a/al + place |
| u05.l02 | ¿Dónde queda? | prepositions and adverbs of place; quedar |
| u05.l03 | Siga derecho | usted commands for directions |
| u05.l04 | En el metro | subirse a, bajarse de, llegar a; transport |
| u06.l01 | Todos los días | regular present tense; hacer |
| u06.l02 | Empiezo a las nueve | stem-changing verbs |
| u06.l03 | ¿Qué hora es? | telling time; days of the week |
| u06.l04 | Me levanto temprano | reflexive verbs; ir vs irse |
| u07.l01 | ¿A cómo el kilo? | demonstratives este, ese, aquel; esto, eso |
| u07.l02 | ¿Me la puedo probar? | direct object pronouns lo, la, los, las |
| u07.l03 | ¿Me lo deja más barato? | comparatives; mejor, peor; numbers to mil |
| u08.l01 | ¿Qué vas a hacer? | ir a + infinitive |
| u08.l02 | ¿Cómo está el clima? | weather with hacer, estar, llover |
| u08.l03 | ¿Qué estás haciendo? | present progressive |
| u09.l01 | ¿Qué hiciste ayer? | regular preterite; ya + preterite |
| u09.l02 | Fui a Oaxaca | irregular preterites |
| u09.l03 | ¿Qué pasó? | narrating in order; impersonal they |
| u10.l01 | Cuando era niño | imperfect: habits and descriptions |
| u10.l02 | En la secundaria | imperfect: age, time, antes vs ahora |
| u10.l03 | Cuando tembló | preterite and imperfect together |
| u11.l01 | Me duele la cabeza | doler like gustar |
| u11.l02 | ¿Qué tiene? | tener que, deber, sentirse |
| u11.l03 | ¡Auxilio! | usted/ustedes commands, irregular and with pronouns |
| u12.l01 | Entre cuates | informal and vulgar register |
| u12.l02 | Un ratito | diminutives; courtesy formulas |
| u12.l03 | Palabras del náhuatl | Nahuatl loanwords; tl and x |

Grammar scope follows PLAN.md §7.4. Present tense runs through Unit 8, with
ir a + infinitive and the progressive in Unit 8. The preterite starts in Unit 9
and the imperfect in Unit 10. Formal commands come in Unit 5 (directions) and
Unit 11 (health). The subjunctive appears only inside commands and fixed
formulas, and vosotros never appears.

Levels: Units 2–6, 7.1 and 8 are A1; 7.2–7.3 and Units 9–12 are A2. Lexicon
rows are A1 through Unit 8 and A2 from Unit 9.

## Counts

| Unit | Lessons | New lemmas | of which rows in core.tsv | Rows in uNN.tsv | of which supporting | Names |
|---|---|---|---|---|---|---|
| 2 | 4 | 48 | 8 | 72 | 32 | 3 |
| 3 | 4 | 48 | 15 | 50 | 17 | 1 |
| 4 | 4 | 48 | 8 | 47 | 7 | 2 |
| 5 | 4 | 48 | 3 | 55 | 10 | 8 |
| 6 | 4 | 48 | 1 | 63 | 16 | 0 |
| 7 | 3 | 36 | 1 | 45 | 10 | 0 |
| 8 | 3 | 36 | 4 | 49 | 17 | 0 |
| 9 | 3 | 36 | 0 | 36 | 0 | 2 |
| 10 | 3 | 36 | 1 | 35 | 0 | 0 |
| 11 | 3 | 36 | 1 | 36 | 1 | 1 |
| 12 | 3 | 36 | 1 | 35 | 0 | 0 |
| **Total** | **38** | **456** | **43** | **523** | **110** | **17** |

"Rows" excludes names. Every lesson introduces 12 lemmas. With Units 0–1 (6
lessons, 68 lemmas) the course has 44 lessons and 524 lesson lemmas.

Unit 2 has the most supporting rows because it carries the function words
(que, o, porque, del, al, las...) and the numbers 16–29. The numbers 16–19 and
21–29 are supporting rather than introduced: they follow the dieci- and veinti-
patterns, and introducing them would spend two lessons on numbers. Months
(u08.tsv) are supporting for the same reason. Days of the week are introduced
(u06.l03).

**Core words introduced late.** Units 0–1 and their story and phrasebook use
94 core.tsv words that no lesson has introduced (comer, estar, familia,
amigo...). The plan introduces 43 of them where they belong (estar in u02.l03,
comer in u03.l01...). The other 51 stay supporting: function and question
words, expressions such as más o menos and un poco, señor/señora/señorita,
español/inglés, chocolate, leche, fonda, saludar, bienvenido, despacio.
`check_plan.py` skips Units 0–1 unless run with `--all`.

## Recurring cast

Names already in names.tsv: Tom (American, studies Spanish in Mexico City),
Ana (from Chicago, lives on calle Xola), Sofía (Mexican engineer, Tom's friend),
Luis (from Monterrey), Laura (doña Laura, the older neighbor), Diego (new
neighbor, later Mariana's coworker), Elena (maestra), Toño (waiter at the fonda),
Ruiz (la doctora Ruiz). New rows: Mariana (Sofía's coworker, u02), Chuy
(taquero, u03), Memo (Sofía's younger brother, u04), Lupe (doña Lupe, landlady
and market vendor, u04), Beto (don Beto, pesero driver, u05).

## Linker traps

These come from a scan of the merged lexicon against the current compiler.
The compiler reports each one, so none is silent, but writers should expect
them.

- **Same headword** (warns; mark the key): `bueno` / `bueno#hello`;
  `mañana` (tomorrow) / `mañana#morning`; `tarde` (afternoon) / `tarde#late`.
- **Headword that is also another lemma's form** (warns, except right after a
  determiner as in "la cocina"; mark the one you mean): trabajo/trabajar, cocina/cocinar, cena/cenar, desayuno/desayunar,
  cambio/cambiar, como/comer, ganas/ganar, mal/malo, mande/mandar,
  sal/salir, viaje/viajar.
- **Real ambiguities** (errors until marked): fue, fui, fuimos, fuiste,
  fueron (ser or ir); di (dar or decir); sé (saber or ser); ve, ven (ir,
  venir or ver), and ve + pronoun (velo, veme).
- **Resolved by rule** (check the link line): a reflexive verb only after a
  matching reflexive pronoun, even over a noun (me llamo vs te llamo; me visto vs ¿viste?; me baño vs el baño);
  an adjective or noun over a participle (la semana pasada, estoy perdida,
  las comidas, dé vuelta); a noun after a determiner, a verb otherwise
  (las sales vs ¿a qué hora sales?, which warns); `la`, `los`, `las` right
  before a conjugated verb are object pronouns (`lo`), articles elsewhere.
  Attached pronouns on a verb with a reflexive twin (llamarme, quedarte,
  irme) warn so you confirm.
- **Object pronoun side effect**: "la ayuda" or "las compras" would make
  `la`/`las` pronouns, because ayuda and compras exist only as verb forms.
  Avoid them or add noun rows. "la cena" and "el desayuno" are fine: both are
  noun rows (u06, supporting).

## Words left out on purpose

| Word | Why | Instead |
|---|---|---|
| `sale` (OK) | every `sale` of salir would warn | notes only; `va`, `órale` |
| `¡aguas!` (watch out) | every plural `aguas` would warn | notes only |
| `ayuda` (noun) | every `¿me ayuda?` would warn | `auxilio`; "¡Ayuda!" links to ayudar |
| `enfermarse` | removed while `enferma` was ambiguous with enfermo; the compiler now resolves it, so it can come back | `estar enfermo` |
| `bajo` (short, low) | would shadow `(me) bajo` of bajarse | `chaparro` |
| `poner`, `lavar`, `probar` | their reflexive twins carry the lesson | `ponerse`, `lavarse`, `probarse` |
| `entrar` | `entre` (preposition) collision | `pasar` (pase) |
| `parar` | `para` collision | `parada`, `bajarse` |
| `moreno`, `gordo` | describing bodies and skin is a register hazard at A1 | `alto`, `chaparro`, `güero` |
| vulgar words other than `pinche` | kept to one recognition item | notes mention `no mames` |

## Requests to the lead (files the plan does not own)

- `content/lexicon/core.tsv`: add "to happen" to the gloss of `pasar` (now
  "to pass; to come in; to go by"). u09.l03 is built on "¿Qué pasó?".
- core.tsv `cuánto` is an adverb with no forms. The plan adds `cuántos` (det,
  fpl `cuántas`) as a separate row, so "¿cuánta agua?" stays unlinkable. A
  `forms` entry on core `cuánto` would need its pos changed to det.
- core.tsv `una` has no plural; the plan adds `unas` as its own row (u02).
- Units 0–1 already had their new warnings marked (mañana, baño, Mande,
  cocina) by the time this plan was finished.

## For the reviewer

Judgement calls about Mexican usage that a native speaker should confirm:

1. **Register tags.** `güero` informal (as the style guide says), while
   `chaparro`, `tantito`, `ni modo`, `a sus órdenes`, `prepa`, `tele`,
   `apapachar` and `tocayo` are neutral. `güey` informal rather than vulgar.
   `mitote`, `chavo`, `neta`, `chamba`, `lana`, `cuate` informal. `pinche` vulgar.
2. **Including `pinche`** as the one vulgar recognition item (hidden by
   default), with `no mames` mentioned only in a note.
3. **"Estoy casado" vs "soy casado"**: the plan calls estar the commoner form.
4. **"Jugar futbol"** without "al" as the Mexican default; "futbol" spelled and
   stressed on the last syllable (`foot-BOHL`).
5. **Time**: "cuarto para las dos", "diez para las tres" taught as the Mexican
   way to say minutes before the hour.
6. **Phone verbs**: marcar ("te marco"), llamar, and hablar for phoning ("te
   hablo") are all taught as everyday Mexican.
7. **"¿Me regala...?"** as a neutral request formula for "can I have", and
   whether it is regional (central Mexico).
8. **"Atrás de"** preferred to "detrás de"; **"enfrente de"** to "delante de".
9. **Words for things**: recámara and cuarto for bedroom; azotea (roof
   terrace); regadera (shower); estufa (stove); clóset (with accent);
   playera; chamarra; tenis; pesero (Mexico City) vs micro; camión for city bus;
   caricatura (cartoon); gripa; consultorio.
10. **Market Spanish**: "¿A cómo el kilo?", "¿Me lo deja en...?", marchante /
    marchanta, "Va" to close a deal.
11. **Meals**: la comida at 2–4 p.m., translated as "lunch" or "midday meal";
    la cena as a light evening meal.
12. **"Te invito"** meaning "my treat".
13. **Nahuatl etymologies** used in notes: cuate (coatl), tocayo (tocaitl),
    apapachar (papachoa), mitote (mitotl), papalote (papalotl), guajolote,
    molcajete, chapulín, cacahuate, atole, tamal, elote, mole, tianguis. The
    first four are the least certain.
14. **Earthquake content** in u10.l03 (alerta sísmica, simulacro on 19
    September, "No grito, no corro, no empujo"): tone and facts.
15. **Candelaria** (2 February tamales after the rosca of 6 January) as the
    u12.l03 dialogue scene.
16. **anteayer** taught with "antier" mentioned as the common Mexican form.

## Validation

```sh
python3 tools/packc --check --allow-missing-examples
# packc: check passed: 726 lemmas, 127 verb tables, ...; only [example] and [review] warnings
python3 content-plan/check_plan.py --counts
# check_plan: 0 problem(s)
```

A pack was built once from a scratch copy of `content/` (never the real
`ids.lock`) to read every respelling, inflection and verb table of the new rows
in the dump. Corrections from that pass (`clósets`, the three verb entries) are
in.

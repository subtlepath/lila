# Frequency deck: plan

The word list for Tinta's frequency deck: the high-frequency words a learner
meets after the 44 lessons (PLAN.md §4.1, §7.4–7.5). It follows
`docs/content-style.md` section 6 ("The frequency deck") and section 7
(`deck/NAME.yaml`). This plan decides which words are in the deck, in what
order, and who writes their sentences. It contains no sentences.

| Path | What |
|---|---|
| `content/lexicon/frequency.tsv` | 1,006 deck words in `freq` order, with complete rows |
| `content/verbs/irregular/frequency.yaml` | 6 verbs the conjugator needed |
| `content/pronunciation/frequency.yaml` | 2 respelling overrides (video, boiler) |
| `content-plan/deck/batches.yaml` | The three writing batches: A, B and C, each with its keys |
| `content-plan/deck/check_deck.py` | Checks the batches, and the deck files written so far |

## For deck writers

You write `content/deck/a-*.yaml` (batch A), `b-*.yaml` (batch B) or
`c-*.yaml` (batch C), and no other files. Split your batch into files of about
50 words in `freq` order, for example `a-01.yaml` (freq 1–50), `a-02.yaml`
(51–100), and so on. The `a-`/`b-`/`c-` prefix tells the checker which batch a
file belongs to, and keeps deck names apart from story and phrasebook names.
The file format is in style guide section 7: `sentences:` with `es`, `en` and
optional `note`, `order`, `allow`, `id` and `reviewer_notes`, plus an optional
`title:`. Do not edit `frequency.tsv` or the batches; send gloss fixes to the
lead.

### Rules for deck sentences

1. **Targets.** Write one or two sentences for every word in your batch. Each
   sentence marks exactly one target, `{word}`, and the target must be a word of
   your own batch. The cloze item comes from that mark. Mark the word where it
   appears, in any form: `{preocupes|preocuparse}`, `{lindas}`.
2. **Vocabulary.** A sentence may use:
   - any course word: the rows of `core.tsv`, `u02.tsv`–`u12.tsv` and `names.tsv`;
   - any deck word of your batch or of an earlier batch (more frequent words).
     Batch A may use A only, batch B may use A and B, and batch C may use
     everything.

   Do not use deck words from a later batch. Do not use words that are only in
   `phrasebook.tsv` (suadero, cajuela, garrafón...) or a `dictionary*.tsv`: the
   learner has no card for them. If a sentence plainly needs a missing word, tell
   the lead. Do not add lexicon rows yourself.
3. **Grammar.** You may use what the course teaches: the present, `ir a` +
   infinitive, the present progressive, the preterite and the imperfect, object
   and reflexive pronouns, and commands (`usted`, `tú`, negative). The simple
   future and the conditional are allowed sparingly, in batch C only. The
   subjunctive is allowed only in fixed formulas (`¡Que te alivies!`,
   `¡Que te vaya bien!`, `Ojalá que sí`). Use the preterite for completed past,
   not the present perfect (`Hoy comí tacos`).
4. **Mexican Spanish** (style guide sections 8–9). Write everyday speech of
   Mexico City, Guadalajara or Monterrey. Use usted with strangers, staff and
   elders, and tú with friends and family. Spell numbers out. When a sentence
   uses an `informal` word (tonto, onda, panza, gringo...), make the scene
   informal and add `note: "Informal: only with friends."`.
5. **Length.** Aim for 12 words or fewer, under about 90 characters. The
   checker notes anything over 100 characters; the compiler's hard limit is 132
   characters of Spanish and 172 of English.
6. **One right answer.** The Spanish and the English together must leave
   exactly one cloze option correct (style guide section 11). Build a scratch
   pack and read every cloze in the dump with its options.
7. **Word order.** You may give `order: true` to about one sentence in four,
   with 3 to 8 words.
8. Use `reviewer_notes:` for judgement calls about usage.

### Linker traps in the deck words

The compiler reports each of these, but here is what to expect:

- **Homographs:** each pair is the neutral sense (the default) and a second
  key. Mark every use with its key, since an unmarked one warns:
  - `claro` (of course!) and `claro#light` (light-colored): `azul [claro|claro#light]`;
  - `banda` (band, gang) and `banda#friends` (your friends, informal);
  - `mordida` (a bite) and `mordida#bribe` (informal);
  - `codo` (elbow) and `codo#stingy` (adjective, informal);
  - `chueco` (crooked) and `chueco#shady` (adjective, informal). `chueca` and
    `chuecos` are ambiguous errors until they are marked.
- **Marked-only word:** `solo#alone` (alone) has `link: marked`, so nothing
  links to it unless you mark it. Unmarked `solo` stays the course adverb (only,
  just), and an unmarked `sola`/`solos`/`solas` is an unlinked-word error. Write
  `[sola|solo#alone]` and `{solo|solo#alone}`.
- **Headword beats a participle:** `hecho` (fact) wins over hacer's participle,
  so `hecho a mano` needs `[hecho|hacer]`. `seguido` (often) warns against
  seguir. An adjective or noun beats a participle silently, which is usually what
  you want: `está cerrado`, `abierto`, `dormido`, `roto`, `despierto`,
  `equivocado`, `enamorado`, `mojado`, `parecido`, `muerto`, `invitado`,
  `vestido`, `llegada`, `estado`.
- **Plural noun or verb form:** without a determiner before them, `mejores`
  links to mejorar (mark `[mejores|mejor]`) and `estaciones` links to
  estacionar. `recibo` is a noun (receipt, bill), so the verb in `Recibo...`
  needs `[Recibo|recibir]`. The same goes for `cuento`, a noun (story) and
  contar's yo form: `Te [cuento|contar] algo`. After a determiner (`un cuento`)
  it is the noun without marking. `cuenta` is the course noun (the bill), so the
  verb needs `[cuenta|contar]`.
- **Verb and reflexive twin:** these pairs now exist: acabar/acabarse,
  arreglar/arreglarse, lavar/lavarse, romper/romperse, subir/subirse,
  pegar/pegarse, preocupar/preocuparse, sentir/sentirse, poner/ponerse,
  probar/probarse, caer/caerse. After its own reflexive pronoun (`me preocupo`,
  `se rompió`), a form links to the reflexive verb. Anywhere else (`me preocupa`,
  `lo siento`) it links to the plain verb. **Se me** sentences are the exception:
  `Se me rompió`, `se me acabó`, `se me cayó` link to the plain verb. Mark them
  `[rompió|romperse]`.
- **Gustar-type verbs** (importar, faltar, interesar, molestar, preocupar,
  antojarse) link correctly after `me te le nos les`.
- **Reflexive forms that look like other words:** `se para` is pararse,
  `se casa` is casarse and `me río` is reírse. Elsewhere `para`, `casa` and `río`
  stay the preposition and the nouns.
- **Multi-word headwords** (a lo mejor, tal vez, de acuerdo, o sea, en serio,
  por supuesto, por cierto, por fin, sin embargo, para nada, cómo no, con gusto,
  con razón, de veras, de nuevo, a poco, ya mero, de volada, buena onda, mala
  onda, al rato, nada más, pasado mañana, así que, por ejemplo) match automatically only between punctuation.
  Inside a clause, mark them: `Nos vemos [pasado mañana|pasado mañana]`.
- `donde` and `quien` are the relative words, without an accent; `dónde` and
  `quién` ask questions.

### Checking your batch

```sh
python3 tools/packc --check --allow-missing-examples       # no errors, and no warnings in your files
python3 content-plan/deck/check_deck.py A                  # your batch letter
# read your clozes in a scratch build (never the real content/: it would write ids.lock)
rm -rf build/deck-A && mkdir -p build/deck-A && cp -R content build/deck-A/content
python3 tools/packc --content build/deck-A/content --out build/deck-A/course.pack --dump --allow-missing-examples
```

`check_deck.py` verifies the batches and every written deck file. Each word of
a batch with files must be the target of one or two sentences. Until your batch
is finished, the "target of no sentence" lines list the words still to write. A sentence must
have exactly one target from its own batch and no word from a later batch, a
phrasebook row or a dictionary row. Batches A and B may not use the future or
the conditional. It lists each subjunctive form for you to confirm that it is a
fixed formula, and notes sentences over 100 characters.

## How the list was made

1. **Source.** Ranking comes from the OpenSubtitles 2018 Spanish list
   `content/2018/es/es_50k.txt` of
   [hermitdave/FrequencyWords](https://github.com/hermitdave/FrequencyWords)
   (word forms with counts, built from the OPUS OpenSubtitles2018 corpus).
   **Licence: CC BY-SA 4.0 for the list content** (the repository's code is
   MIT). The list was downloaded to `build/freq/`, which is not committed. Only
   the order of our own lemmas comes from it; none of its data is copied. See
   the requests below about attribution.
2. **Candidates.** A pool of about 1,100 lemmas was chosen by judgement. It came
   from the frequent word forms the existing lexicon did not cover, plus everyday
   Mexican words a subtitle list under-represents: food, the household, city
   life, work, school, health, feelings and time.
3. **Lemmatising and counting.** Each candidate was put into a scratch copy of
   `content/`. The compiler's own conjugator and inflector generated every form
   (verb tables, plurals, feminines, attached pronouns). A lemma's score is the
   sum of the subtitle counts of all its forms. A form shared by several lemmas
   is split between them, and a headword beats an inflected form (`como`, the
   conjunction).
4. **Adjustments.** Subtitles over-represent crime, fantasy and swearing, and
   under-represent daily life. So the score was capped for subtitle-heavy words
   (matar, muerte, dios, the participle-inflated `hecho` and `estado`) and given
   a floor for everyday words: four bands for Mexican words such as acá, checar,
   quincena, tope and tinaco, and concrete basics such as arroz, cuchara, jabón
   and the seasons. `freq` is the rank after these adjustments, 1 to 999. Four
   rows added after the batches were handed out share the `freq` of a neighbour
   and keep the batch ranges fixed: contar (54, batch A; it had been
   missing); solo#alone (100, batch A; `link: marked`, see the traps); pegarse
   (503, batch B; handed over by the phrasebook); and the slang senses
   banda#friends (batch B) and mordida#bribe, codo#stingy and chueco#shady
   (batch C), split from their neutral words because the guide gives a
   colloquial sense its own key.
5. **Selection and cuts.** Names, interjection noise (oh, eh, hey), vulgar
   words, Peninsular-only words and low-value subtitle words were removed. About
   140 more were cut, which left 915.
6. **Words moved from the phrasebook.** `phrasebook.tsv` rows make no
   vocabulary items, so 84 general words moved from it into the deck, agreed with
   the lead and the phrasebook author. Examples: verdad, parecer, poner, sacar,
   cada, alguno, teléfono, salida, sopa, al rato. Deck sentences may use them
   like any other deck word of their batch. Their glosses, notes and forms are the
   phrasebook author's, so the senses their phrases use are kept. I added
   topics, syn and a few notes. They were ranked the same way. Phrase-specific
   words stay in `phrasebook.tsv`: suadero, cajuela, garrafón, transbordar and
   the like. From now on, a general word the phrasebook needs is added to the
   deck.
7. **Linker safety.** Every candidate was compiled against the existing content.
   The compiler messages and the link of every existing sentence, dialogue line,
   story line and phrase were compared before and after (see "Effect on existing
   content").

### Counts

| | Words |
|---|---|
| Nouns | 613 |
| Verbs | 181 |
| Adjectives | 126 |
| Adverbs | 28 |
| Expressions and interjections | 29 |
| Determiners, pronouns, prepositions, conjunctions | 23 |
| Numbers (trescientos to novecientos) | 6 |
| **Total** | **1,006** |

Levels: 74 A1, 839 A2, 93 B1. 193 words carry the Mexico flag and 25 are
`informal`; none is vulgar. Batch A covers freq 1–333 (335 words), B 334–666
(335) and C 667–999 (336).

## Words left out on purpose

| Word | Why | Instead |
|---|---|---|
| vino (wine) | `vino` is the preterite of venir, used in Unit 9 | none |
| cara (face) | the feminine of caro (`muy cara`) | none |
| juego, regalo, pregunta (nouns) | forms of jugar, regalar, preguntar used in lessons | |
| limpio, vivo, extraño (adjectives) | forms of limpiar, vivir, extrañar used in lessons | sucio |
| renta (noun) | rentar's `renta`, used in Unit 4 | rentar |
| llenar, calentar | `lleno` and `caliente` (adjectives in lessons) would warn | |
| nadar | `nada` everywhere would warn | |
| andar | `ándale` is andar + le | |
| crear | `creo`, `cree` would become ambiguous with creer | |
| sentar, sentarse | `me siento` would become ambiguous with sentirse | `pararse` (stand up) |
| bajar | relinks `¡Bajan!` (u05.l04 dialogue, u05 pesero story) from bajarse | could come back once those two lines are marked `[Bajan\|bajarse]` |
| cortar | `corta`/`corto` clash with the adjective corto | cortarse (Unit 11) |
| falta (noun), junta (meeting), duro | `me falta` (faltar); `juntas` (junto); `dura` (durar) | faltar, reunión, durar |
| sobrar, saltar, tender, vela, interés | `sobre`, `salte` (sal + te), `tienda`, `ve` + `la`, `intereses` (interesar) | brincar |
| entrar, parar | as in the course plan: `entre`, `para` | pararse |
| padre (father), frío (noun) | homographs of padre#cool and the adjective frío in lessons | madre is in |
| otra vez, a veces, a ver, por eso | already used word by word in lessons; a multi-word headword would warn there | |
| tener ganas, darse cuenta, hacer falta | a multi-word verb phrase cannot match its conjugated forms | ganas, faltar |
| one of each noun/verb pair | abrazo/abrazar, beso/besar, sueño/soñar, almuerzo/almorzar, seco/secar, dibujar/dibujo, pelear/pelea, divorcio/divorciarse... keep both only where the forms do not clash | |
| documento, embarazada | the phrasebook's documentar and embarazado | |
| vulgar and Peninsular words | mierda, joder, cabrón...; coger, vale, ordenador, móvil, piscina, conducir | |

## Effect on existing content

`python3 tools/packc --check --allow-missing-examples` passes. The only new
warnings are `[example]` warnings on deck rows; about 85 rows already have an
example, most of them from phrases. Moving the 84 rows out of `phrasebook.tsv`
changes no link: phrases link by key, whichever file holds the row. Compared with the content without the deck, two existing links
change, both without a warning:

- `units/03-taqueria/lesson-3.yaml` dialogue line 3, "Sí, cómo no.": `cómo no`
  is now the expression (it was `cómo` + `no`). This is the intended reading.
- `phrasebook/salud.yaml` phrase 14, "¿Hay alguna farmacia abierta?": `abierta`
  is now the adjective abierto (it was abrir's participle). This is better.

A third, `phrasebook/tramites.yaml` "Se me acabaron los datos.", would have
moved from acabarse to acabar. The phrasebook author has since marked it
`[acabaron|acabarse]`.

A scratch pack was built from a copy of `content/` with `--dump`. Every
respelling, plural, feminine and verb table was read there: the 915 original rows
first, then the 84 moved rows.
Corrections from that pass are in: no plural for uncountables and compass points
(`pl=-`), `f=` for actriz, madrina, comadre, presidenta and escuincla, and
`menús` and `boilers`.

## Requests to the lead

1. **Attribution** (the lead handles it). Add FrequencyWords (Hermit Dave;
   CC BY-SA 4.0; derived from OpenSubtitles via OPUS) to `NOTICE`, and decide
   whether a `freq` order derived from a CC BY-SA list makes the pack "adapted
   material" under ShareAlike.
2. **`tools/packc/data/verb_patterns.tsv`** (passed to m3-pack): add
   `aventar e>ie`. `irregular/frequency.yaml` already declares the verb, but the
   guard list would not catch a future aventar that lacks an entry.
3. **Forms deck writers will want:** `gran`, `quiénes`, `miles` and `cientos`
   were already in place: core.tsv (`grande` apoc=gran, `quién` pl=quiénes) and
   u07.tsv (`mil` pl=miles, `ciento` pl=cientos). No change was needed.

## For the reviewer

Judgement calls about Mexican usage that a native speaker should confirm:

1. **Register.** These are tagged `informal`: tonto, panza, antro, gringo,
   chilango, chamaco, escuincle, morro, carnal, compa, onda, rollo, mala onda,
   chafa, gacho, chipote, banda#friends, mordida#bribe, codo#stingy, chueco#shady, de volada,
   guácala; and, from the phrasebook,
   buena onda, bronca and chela. These are left
   `neutral`: nomás, a poco, o sea, ya mero, aventar, pesado
   (annoying), feria (change). Is gringo rude enough to be avoided?
2. **Gender.** el sartén (Mexico) or la sartén; la pijama; el azúcar; la combi;
   el internet.
3. **Mexican senses taught.** ocupar = to need (¿Qué ocupas?); pararse = to
   stand up; voltear = to turn or look back; jalar = to pull; aventar = to throw,
   conjugated aviento; checar; apurarse; aliviarse = to get better; coraje =
   anger; apenado = embarrassed; seguido = often; diario = every day; con gusto
   as the reply to gracias; nieve = sorbet; agua natural = not chilled; pelo
   chino = curly hair; detalle = small gift; contacto = wall socket; timbre also
   = postage stamp; plaza = shopping mall; ruta = city bus line.
4. **Words for things.** tinaco, boiler (water heater), cochera, excusado,
   foco, cobija, traste(s), cubeta, chapa, tapete, lavabo, licuadora, comal,
   refri; combi vs micro vs pesero; credencial (INE); tránsito (traffic
   police); caseta; glorieta; tope; crucero; lonche (northern?); esquites;
   tostada; bolillo; concha.
5. **Respellings.** video stressed `BEE-deh-oh`; boiler `BOY-lehr`; chofer
   stressed on the last syllable; beisbol `bays-BOHL`; kínder `KEEN-dehr`.
6. **Facts in notes.** the altitude of Mexico City (2,240 m); the ajolote on
   the fifty-peso bill; the Mundial in 1970, 1986 and 2026; posadas from 16 to
   24 December; ofrendas on 1–2 November; pozole on 15 September; the
   aguinaldo required by law; the INE voter card as ID.
7. **Respeller:** `deuda` comes out `DEH-oo-dah` and `reunión`
   `rreh-oo-NYOHN`, as if *eu* were two syllables. Is that close enough, or
   should the respeller treat *eu* as one syllable (`DEHW-dah`)?
8. **coche.** It stays in the deck with the note "carro is the everyday word;
   coche is common in Mexico City". Is that right?

"""The compiler proper: sources -> linked, validated course model ready to emit."""

from __future__ import annotations

import os
import re
import unicodedata
from dataclasses import dataclass, field

from . import forms as F
from . import markup, phon, yamlio
from .conjugate import ConjugationError, Conjugator
from .diag import Diagnostics, Where
from .fold import fold
from .respell import Respeller
from .sources import (GENDERS, LEVELS, NOTE_KINDS, POS, REGISTERS, STORY_KINDS, CourseSrc,
                      LessonSrc, LexEntry, SentenceSrc, StorySrc, load_course)

NONE16 = 0xFFFF
WORD_RE = re.compile(r"[0-9A-Za-zÀ-ÖØ-öø-ÿ]+")
CLITICS = ("me", "te", "se", "lo", "la", "le", "nos", "los", "las", "les")
CLITIC_PAIRS = [(a, b) for a in ("me", "te", "se", "nos") for b in ("lo", "la", "los", "las")]

# Lemma flags (docs/pack-format.md 3.5)
LF_MEXICO, LF_PLURAL_ONLY, LF_EL_FEMININE, LF_HAS_ITEMS, LF_FREQUENCY, LF_CONFUSABLE, LF_DICTIONARY = (
    1, 2, 4, 8, 16, 32, 64)
# Token flags (3.8)
TF_TARGET, TF_MULTIWORD, TF_NUMBER, TF_NAME = 1, 2, 4, 8
# Sentence sources and flags
SRC_LESSON, SRC_DIALOGUE, SRC_STORY, SRC_PHRASE, SRC_DECK = range(5)
SF_HAS_TARGET, SF_WORD_ORDER = 1, 2

# In spanish_texts allow sets: digits are fine here (notes quote addresses and
# street names such as "5 de Mayo"; they are not drilled or respelled).
ALLOW_DIGITS = "#digits"

STEM_CLASSES = {"": 0, "e>ie": 1, "o>ue": 2, "e>i": 3, "u>ue": 4, "i>ie": 5}


_PATTERNS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "verb_patterns.tsv")
_SUGGEST = {"irregular": "its irregular forms (see the file's header)", "accent": "{accent: true}",
            "e>i accent": "{stem: e>i, accent: true}"}


def known_verb_patterns() -> dict[str, str]:
    """tools/packc/data/verb_patterns.tsv: verb or -ending -> pattern."""
    out: dict[str, str] = {}
    with open(_PATTERNS_PATH, encoding="utf-8") as fh:
        for line in fh:
            if line.strip() and not line.startswith("#"):
                verb, pattern = line.rstrip("\n").split("\t")
                out[verb] = pattern
    return out


def verb_pattern(verb: str, patterns: dict[str, str]) -> tuple[str, str] | None:
    """(pattern, family root) for a non-reflexive infinitive the rules alone would get wrong.

    An exact entry wins; otherwise the longest family ending (-tener matches
    mantener). The root is the ending without "-" (tener), or the verb itself.
    """
    if verb in patterns:
        return patterns[verb], verb
    endings = [e for e in patterns if e.startswith("-") and verb.endswith(e[1:])]
    if not endings:
        return None
    ending = max(endings, key=len)
    return patterns[ending], ending[1:]


def nfc(text: str) -> str:
    return unicodedata.normalize("NFC", text)


@dataclass
class Lemma:
    key: str
    entry: LexEntry
    es: str
    en: str
    pos: int
    gender: int
    plural_only: bool
    level: int
    reg: int
    flags: int = 0
    pron: str = ""
    feminine: str = ""
    plural: str = ""
    verb: object | None = None          # Conjugation
    verb_table: int = NONE16
    forms: list[tuple[str, int]] = field(default_factory=list)  # inflections, base excluded
    fold: str = ""
    id: int = NONE16
    lesson: int = NONE16
    examples: list[int] = field(default_factory=list)
    target_examples: list[int] = field(default_factory=list)
    senses: set[str] = field(default_factory=set)
    words: tuple[str, ...] = ()         # headword as lower-case words, without ¿?¡! (¿qué tal? -> qué, tal)
    synonyms: set[str] = field(default_factory=set)  # lemma keys never offered as its distractors

    def form_for(self, tag: int) -> str | None:
        if tag == F.BASE or (tag == F.NONFINITE["inf"] and self.verb is not None):
            return self.es
        for form, t in self.forms:
            if t == tag:
                return form
        return None


@dataclass
class Token:
    start: int          # character offsets in the plain text
    end: int
    surface: str
    lemma: Lemma | None
    tag: int
    flags: int
    byte_start: int = 0
    byte_len: int = 0


@dataclass
class Sentence:
    src: SentenceSrc
    es: str             # plain text, NFC
    en: str
    note: str
    tokens: list[Token]
    lesson: int
    level: int
    source: int
    flags: int = 0
    reg: int = 0
    id: int = 0


@dataclass
class Note:
    title: str
    kind: int
    spans: list[tuple[int, str]]
    lesson: int
    where: Where


@dataclass
class Story:
    title: str
    title_en: str
    kind: int
    level: int
    lesson: int
    lines: list[tuple[str, int, int]]          # (speaker, sentence id, flags)
    questions: list[dict]
    src: StorySrc


@dataclass
class Lesson:
    src: LessonSrc
    id: int
    unit: int
    level: int
    new: list[Lemma] = field(default_factory=list)
    notes: list[int] = field(default_factory=list)
    sentences: list[int] = field(default_factory=list)
    dialogue: int = NONE16
    first_item: int = 0
    item_count: int = 0
    reviewed: bool = False
    hash: str = ""


@dataclass
class PhraseEntry:
    sentence: int
    category: int
    pron: str


@dataclass
class Build:
    course: CourseSrc
    diag: Diagnostics
    lemmas: list[Lemma] = field(default_factory=list)          # by id
    by_key: dict[str, Lemma] = field(default_factory=dict)
    verbs: list[Lemma] = field(default_factory=list)            # by verb-table index
    sentences: list[Sentence] = field(default_factory=list)
    lessons: list[Lesson] = field(default_factory=list)
    notes: list[Note] = field(default_factory=list)
    stories: list[Story] = field(default_factory=list)
    phrase_entries: list[PhraseEntry] = field(default_factory=list)
    phrase_categories: list[dict] = field(default_factory=list)
    confusables: list[dict] = field(default_factory=list)
    items: list = field(default_factory=list)                   # items.Item
    spanish_texts: list[tuple[Where, str, frozenset]] = field(default_factory=list)
    english_texts: list[tuple[Where, str]] = field(default_factory=list)
    new_uids: int = 0
    retired_uids: int = 0
    id_lock: object = None
    review_status: list = field(default_factory=list)
    deck_files: list = field(default_factory=list)       # {"src": DeckSrc, "sentences": [ids]}
    untabled_verbs: list = field(default_factory=list)   # dictionary verbs waiting for irregular.yaml
    allow_missing_examples: bool = False   # development only: a lemma without an example warns
    respeller: Respeller | None = None
    conjugator: Conjugator | None = None


class Linker:
    """Surface form -> lemma candidates, built from the lexicon."""

    def __init__(self, build: Build):
        self.build = build
        self.index: dict[str, list[tuple[int, Lemma, int]]] = {}   # lower surface -> (prio, lemma, tag)
        self.multi: dict[str, list[tuple[tuple[str, ...], Lemma]]] = {}
        # Only lemmas that link automatically: a dictionary row (llevarse) must never pull
        # "para llevar" away from the course verb.
        self.twins = {lem.es: lem for lem in build.lemmas if lem.verb is not None and _auto_links(lem)}
        # Lexicon order, so the first of several lemmas sharing a headword is the default.
        for entry in build.course.lexicon.values():
            lemma = build.by_key.get(entry.key)
            if lemma is None or not _auto_links(lemma):
                continue  # dictionary and `link: marked` rows link only where a sentence marks them
            words = lemma.words
            if len(words) > 1:
                self.multi.setdefault(words[0], []).append((words, lemma))
                continue
            if words:
                base_tag = F.NONFINITE["inf"] if lemma.verb else F.BASE
                self._add(words[0], 0, lemma, base_tag)
            for form, tag in lemma.forms:
                self._add(_bare(form).lower(), 1, lemma, tag)
            if lemma.verb:
                for form, tag in self._enclitic_forms(lemma):
                    self._add(form.lower(), 2, lemma, tag)
        for entries in self.multi.values():
            entries.sort(key=lambda e: -len(e[0]))

    def _add(self, surface: str, prio: int, lemma: Lemma, tag: int) -> None:
        bucket = self.index.setdefault(surface, [])
        if not any(p == prio and lem is lemma and t == tag for p, lem, t in bucket):
            bucket.append((prio, lemma, tag))

    def _enclitic_forms(self, lemma: Lemma):
        conj = lemma.verb
        bases: list[tuple[str, int]] = []
        plain = conj
        if conj.reflexive:
            try:
                plain = self.build.conjugator.conjugate(conj.infinitive[:-2])
            except ConjugationError:
                plain = None
        if plain is not None:
            bases.append((plain.infinitive, F.NONFINITE["inf"]))
            bases.append((plain.gerund, F.NONFINITE["ger"]))
            for person in F.IMPERATIVE_PERSONS:
                form = plain.forms.get(("imp", person))
                if form:
                    bases.append((form, F.verb_tag("imp", person)))
        for base, tag in bases:
            tag = tag | F.ENCLITIC if F.is_verb_tag(tag) else tag
            for clitic in CLITICS:
                yield phon.attach(base, clitic), tag
            for a, b in CLITIC_PAIRS:
                yield phon.attach(base, a, b), tag

    def lookup(self, surface: str, prev: str = "") -> tuple[Lemma | None, int, str, str]:
        """(lemma, tag, problem, warning). problem is "" when linked.

        prev is the word just before (lower case), "" if punctuation intervenes.
        """
        cands = self.index.get(surface.lower(), [])
        # A reflexive verb's form right after its own pronoun wins, even over a
        # headword (me baño is bañarse, not the noun baño).
        persons = REFLEXIVE_PERSONS.get(prev, ())
        agreeing = [(lem, tag) for p, lem, tag in cands if p == 1 and _reflexive(lem)
                    and F.is_verb_tag(tag) and F.verb_tag_parts(tag)[1] in persons]
        if len({id(lem) for lem, _t in agreeing}) == 1:
            return agreeing[0][0], agreeing[0][1], "", ""
        after_det = self.is_determiner(prev)
        for prio in (0, 1, 2):
            level = [(lem, tag) for p, lem, tag in cands if p == prio]
            if not level:
                continue
            if prio > 0:
                level, warning = _narrow(level, surface, after_det, prio)
                keys = list(dict.fromkeys(lem.key for lem, _ in level))
                if len(keys) == 1:
                    return level[0][0], level[0][1], "", warning
                pair = _reflexive_pair(level)
                if pair and prio == 2:
                    return _pick_reflexive(pair, level, surface)
                return None, 0, f"ambiguous: '{surface}' is a form of {', '.join(sorted(keys))}", ""
            keys = list(dict.fromkeys(lem.key for lem, _ in level))
            winner = level[0][0]
            # A headword also beats forms of other lemmas (trabajo the noun over trabajar
            # pres.1s); say so, since adding a noun later would silently relink old sentences,
            # except where the rules of _narrow would have picked the headword anyway.
            forms_of = list(dict.fromkeys(
                lem.key for p, lem, t in cands if p > 0 and lem.key not in keys
                and not _headword_obviously_wins(winner, lem, t, after_det)))
            warning = ""
            if len(keys) > 1:
                warning = (f"'{surface}' is linked to '{keys[0]}' by default but is also the headword of "
                           f"{', '.join(repr(k) for k in keys[1:])}; confirm with [{surface}|{keys[0]}] or "
                           f"[{surface}|{keys[1]}]")
            elif forms_of:
                warning = (f"'{surface}' is linked to '{keys[0]}' by default but is also a form of "
                           f"{', '.join(repr(k) for k in forms_of)}; confirm with [{surface}|{keys[0]}] or "
                           f"[{surface}|{forms_of[0]}]")
            return winner, level[0][1], "", warning
        return None, 0, f"unlinked word '{surface}': add it to the lexicon or mark it [{surface}|lemma]", ""

    def reflexive_twin(self, lemma: Lemma) -> Lemma | None:
        """quedarse for quedar (or quedarse itself), if the lexicon has it."""
        if _reflexive(lemma):
            return lemma
        return self.twins.get(lemma.es + "se")

    def plain_twin(self, lemma: Lemma) -> Lemma | None:
        """quedar for quedarse, if the lexicon has it."""
        return self.twins.get(lemma.es[:-2]) if _reflexive(lemma) else lemma

    def is_determiner(self, word: str) -> bool:
        return any(lem.pos == POS["det"] for _p, lem, _t in self.index.get(word, []))

    def tags_for(self, lemma: Lemma, surface: str) -> list[int]:
        low = surface.lower()
        tags = [tag for p, lem, tag in self.index.get(low, []) if lem is lemma]
        if not tags and not _auto_links(lemma):  # not indexed: read its own forms
            tags = [t for f, t in lemma.forms if _bare(f).lower() == low]
        return tags or ([F.BASE] if words_of(surface) == lemma.words else [])


MONTHS = frozenset("enero febrero marzo abril mayo junio julio agosto septiembre octubre noviembre "
                   "diciembre".split())
FUNCTION_POS = frozenset({POS["det"], POS["pron"], POS["prep"], POS["conj"]})
NOMINAL_POS = frozenset({POS["noun"], POS["adj"]})
PARTICIPLE_TAGS = frozenset(range(0x2002, 0x2006))
REFLEXIVE_PERSONS = {"me": ("1s",), "te": ("2s",), "se": ("3s", "3p"), "nos": ("1p",)}
_PREV_WORD = re.compile(r"([0-9A-Za-zÀ-ÖØ-öø-ÿ]+)\s+$")


def _reflexive_pair(level) -> tuple[Lemma, Lemma] | None:
    """(llamar, llamarse) when the candidates are exactly a verb and its reflexive twin."""
    lemmas = list({id(lem): lem for lem, _t in level}.values())
    if len(lemmas) != 2 or not all(lem.verb is not None for lem in lemmas):
        return None
    plain, refl = sorted(lemmas, key=lambda lem: len(lem.es))
    return (plain, refl) if refl.es == plain.es + "se" else None


def _reflexive(lemma: Lemma) -> bool:
    return lemma.verb is not None and lemma.verb.reflexive


def _narrow(level, surface: str, after_det: bool, prio: int):
    """Drop readings of an inflected form that context rules out; (candidates, warning)."""
    def lemmas(cands):
        return len({id(lem) for lem, _t in cands})

    warning = ""
    if prio == 1 and lemmas(level) > 1:
        # A reflexive verb needs its pronoun right before (lookup takes that case first):
        # viste is ver here, enferma the adjective.
        rest = [(lem, t) for lem, t in level if not _reflexive(lem)]
        level = rest or level
    if lemmas(level) > 1 and any(lem.pos in NOMINAL_POS for lem, _t in level):
        # A participle yields to the adjective or noun spelled the same (la semana pasada).
        rest = [(lem, t) for lem, t in level if not (lem.verb is not None and t in PARTICIPLE_TAGS)]
        level = rest or level
    if lemmas(level) > 1:
        nominal = [(lem, t) for lem, t in level if lem.pos in NOMINAL_POS]
        finite = [(lem, t) for lem, t in level if lem.verb is not None and F.is_verb_tag(t)]
        if nominal and finite and len(nominal) + len(finite) == len(level) and lemmas(nominal) == 1 \
                and lemmas(finite) == 1:
            # las sales (noun) after a determiner, ¿A qué hora sales? (verb) otherwise.
            if after_det:
                level = nominal
            else:
                level = finite
                warning = (f"'{surface}' is linked to the verb '{finite[0][0].key}' (no determiner before "
                           f"it) but could be '{nominal[0][0].key}'; confirm with [{surface}|{finite[0][0].key}] "
                           f"or [{surface}|{nominal[0][0].key}]")
    return level, warning


def _headword_obviously_wins(winner: Lemma, other: Lemma, tag: int, after_det: bool) -> bool:
    """Collisions between a headword and another lemma's form not worth a warning."""
    if winner.pos in FUNCTION_POS and other.pos in FUNCTION_POS and not F.is_verb_tag(tag):
        return True   # la/los/las: settled by _object_pronouns
    if _reflexive(other):
        return True   # needs its pronoun, which lookup checks first
    if winner.pos in NOMINAL_POS and other.verb is not None and tag in PARTICIPLE_TAGS:
        return True   # comida over comer's participle
    if winner.pos in NOMINAL_POS and after_det and F.is_verb_tag(tag):
        return True   # la cocina is the noun
    return _reflexive(winner) and other.es + "se" == winner.es  # irse is not ir + se


def _pick_reflexive(pair, level, surface: str):
    """Attached pronouns (irme, llamarte), shared by X and Xse: me/te/se/nos suggest Xse,
    but "llamarme" can also be "call me", so the author confirms."""
    plain, refl = pair
    bare = phon.strip_accents(surface.lower())
    chosen = refl if bare.endswith(("me", "te", "se", "nos")) else plain
    other = plain if chosen is refl else refl
    warning = (f"'{surface}' is linked to '{chosen.key}' but could be '{other.key}'; confirm with "
               f"[{surface}|{chosen.key}] or [{surface}|{other.key}]")
    return chosen, next(t for lem, t in level if lem is chosen), "", warning


def previous_word(text: str, start: int) -> str:
    """The word right before `start`, lower case, or "" if punctuation intervenes."""
    match = _PREV_WORD.search(text[:start])
    return match.group(1).lower() if match else ""


def _auto_links(lemma: Lemma) -> bool:
    """Whether unmarked words may link to the lemma (not dictionary or `link: marked` rows)."""
    return not (lemma.entry.dictionary or lemma.entry.marked_only)


def words_of(text: str) -> tuple[str, ...]:
    """Lower-case words of a text, punctuation ignored."""
    return tuple(w.lower() for w in WORD_RE.findall(text))


def _bare(form: str) -> str:
    """Drop a reflexive pronoun written before a verb form (`me llamo` -> `llamo`)."""
    head, sep, rest = form.partition(" ")
    return rest if sep and head in ("me", "te", "se", "nos") else form


class Compiler:
    def __init__(self, content_root: str, diag: Diagnostics | None = None,
                 allow_missing_examples: bool = False):
        self.root = content_root
        self.diag = diag or Diagnostics()
        self.allow_missing_examples = allow_missing_examples
        self.patterns = known_verb_patterns()

    # -- lemmas ----------------------------------------------------------------

    def _make_lemmas(self, b: Build) -> None:
        d = self.diag
        for entry in b.course.lexicon.values():
            where = entry.where
            es, en = nfc(entry.lemma), nfc(entry.gloss)
            pos = POS.get(entry.pos)
            if pos is None:
                d.error(where, "schema", f"unknown pos '{entry.pos}' (use {', '.join(POS)})")
                pos = 0
            gender = GENDERS.get(entry.gender)
            if gender is None:
                d.error(where, "schema", f"unknown gender '{entry.gender}' (use m, f, mf, mpl, fpl)")
                gender = (0, False)
            if entry.pos == "noun" and gender[0] == 0:
                d.error(where, "complete", f"noun '{es}' has no gender")
            level = LEVELS.get(entry.level)
            if level is None and entry.dictionary and not entry.level:
                level = 0  # a dictionary headword may have no level
            elif level is None:
                d.error(where, "complete", f"lemma '{es}' has no valid level (A1, A2, B1, B2)")
                level = 0
            reg = REGISTERS.get(entry.register)
            if reg is None:
                d.error(where, "schema", f"unknown register '{entry.register}' (use {', '.join(REGISTERS)})")
                reg = 0
            if not en:
                d.error(where, "complete", f"lemma '{es}' has no gloss")
            lemma = Lemma(key=entry.key, entry=entry, es=es, en=en, pos=pos, gender=gender[0],
                          plural_only=gender[1], level=level, reg=reg, fold=fold(es), words=words_of(es))
            if not lemma.words:
                d.error(where, "schema", f"headword '{es}' has no letters")
            if entry.mx:
                lemma.flags |= LF_MEXICO
            if lemma.plural_only:
                lemma.flags |= LF_PLURAL_ONLY
            if entry.deck:
                lemma.flags |= LF_FREQUENCY
            if entry.dictionary:
                lemma.flags |= LF_DICTIONARY
            self._inflect(b, lemma)
            lemma.senses = english_senses(en)
            b.lemmas.append(lemma)
            b.by_key[entry.key] = lemma
        for lemma in b.lemmas:
            for key in lemma.entry.synonyms:
                other = b.by_key.get(key)
                if other is None:
                    d.error(lemma.entry.where, "link", f"synonym '{key}' is not in the lexicon")
                    continue
                lemma.synonyms.add(other.key)
                other.synonyms.add(lemma.key)
        b.lemmas.sort(key=lambda lem: (lem.fold, lem.es, lem.pos, lem.key))
        for i, lemma in enumerate(b.lemmas):
            lemma.id = i
        b.verbs = [lem for lem in b.lemmas if lem.verb is not None]
        for i, lemma in enumerate(b.verbs):
            lemma.verb_table = i

    def _inflect(self, b: Build, lemma: Lemma) -> None:
        entry = lemma.entry
        over = {k: nfc(v) for k, v in entry.forms.items()}
        # "pl=-" (any tag) says the form does not exist: no generated plural, no plural
        # distractors (fútbol, don). Months have none by default.
        none = {k for k, v in over.items() if v == "-"}
        if entry.pos == "noun" and lemma.es.lower() in MONTHS:
            none.add("pl")
        over = {k: v for k, v in over.items() if v != "-"}
        out: list[tuple[str, int]] = []
        if entry.pos == "verb":
            base = lemma.es[:-2] if lemma.es.endswith("se") else lemma.es
            found = verb_pattern(base, self.patterns)
            if found and base not in b.conjugator.irregular and entry.dictionary:
                b.untabled_verbs.append(base)  # listed in a build note; no table until declared
                return
            if found and base not in b.conjugator.irregular:
                pattern, root = found
                family = "" if root == base else f", like {root}"
                if pattern == "irregular" and root != base and root in b.conjugator.irregular:
                    fix = f"{{like: {root}}}"
                else:
                    fix = _SUGGEST.get(pattern, f"{{stem: {pattern}}}")
                self.diag.error(entry.where, "verb",
                                f"'{base}' is not regular ({pattern}{family}) but content/verbs/irregular.yaml "
                                f"has no entry for it, so its forms would be wrong: add {base}: {fix}, or "
                                f"{base}: {{regular: true}} if it is regular in the sense taught")
            try:
                conj = b.conjugator.conjugate(lemma.es)
            except ConjugationError as exc:
                self.diag.error(entry.where, "verb", str(exc))
                return
            lemma.verb = conj
            for tense in F.TENSES:
                persons = F.IMPERATIVE_PERSONS if tense in ("imp", "impneg") else F.PERSONS
                for person in persons:
                    form = conj.forms.get((tense, person))
                    if form:
                        out.append((form, F.verb_tag(tense, person)))
            for form, (tense, person) in conj.extra.items():
                out.append((form, F.verb_tag(tense, person)))  # hay: haber pres.3s, after ha
            out.append((conj.gerund, F.NONFINITE["ger"]))
            if conj.reflexive:
                # With the pronoun elsewhere in the clause (me voy a quedar, me estoy
                # bañando) the verb appears bare; _climbing_pronouns links it here.
                try:
                    plain = b.conjugator.conjugate(conj.infinitive[:-2])
                    out += [(plain.infinitive, F.NONFINITE["inf"]), (plain.gerund, F.NONFINITE["ger"])]
                except ConjugationError:
                    pass
            part = conj.participle
            out.append((part, F.NONFINITE["part"]))
            if part.endswith("o"):
                out += [(part[:-1] + "a", F.NONFINITE["part.f"]), (part + "s", F.NONFINITE["part.mpl"]),
                        (part[:-1] + "as", F.NONFINITE["part.fpl"])]
        elif entry.pos in ("noun", "adj", "det", "pron", "num"):
            single = " " not in lemma.es or entry.pos in ("noun", "adj")
            fem = over.get("f")
            if fem is None and "f" not in none and single and (
                    entry.pos == "adj" or (entry.pos == "noun" and lemma.gender == 3)):
                fem = F.feminine(lemma.es)
            if fem == lemma.es:
                fem = None
            if lemma.plural_only or none & {"pl", "mpl"}:
                plural = None
            elif "pl" in over or "mpl" in over:
                plural = over.get("mpl") or over.get("pl")
            elif entry.pos in ("noun", "adj") and single:
                plural = F.pluralize(lemma.es)
            else:
                plural = None
            if plural == lemma.es:
                plural = None
            fem_plural = None if none & {"fpl", "pl"} else over.get("fpl") or (F.pluralize(fem) if fem else None)
            lemma.feminine = fem or ""
            lemma.plural = plural or ""
            # Each form stands on its own: cuántos has fpl=cuántas but no separate f.
            gendered = bool(fem or fem_plural or "mpl" in over)
            if fem:
                out.append((fem, F.NOMINAL["f"]))
            if plural:
                out.append((plural, F.NOMINAL["mpl" if gendered else "pl"]))
            if fem_plural:
                out.append((fem_plural, F.NOMINAL["fpl"]))
        if "apoc" in over:
            out.append((over["apoc"], F.APOCOPE))
        seen = set()
        lemma.forms = [(f, t) for f, t in out if (f, t) not in seen and not seen.add((f, t))]
        if entry.pos == "noun" and lemma.gender == 2 and not lemma.plural_only:
            lemma.flags |= LF_EL_FEMININE if _takes_el(lemma.es) else 0

    # -- sentences ------------------------------------------------------------

    def _sentence(self, b: Build, linker: Linker, src: SentenceSrc, lesson: int, level: int,
                  source: int) -> Sentence | None:
        d = self.diag
        try:
            text, marks = markup.parse_sentence(nfc(src.es))
        except markup.MarkupError as exc:
            d.error(src.where, "markup", str(exc))
            return None
        tokens: list[Token] = []
        covered = [False] * len(text)
        automatic: set[int] = set()  # starts of tokens whose lemma the compiler chose
        for mark in marks:
            surface = text[mark.start:mark.end]
            if not (WORD_RE.match(surface[0]) and WORD_RE.match(surface[-1])):
                d.error(src.where, "markup", f"marked text '{surface}' must start and end with a letter")
                continue
            if (mark.start > 0 and WORD_RE.match(text[mark.start - 1])) or (
                    mark.end < len(text) and WORD_RE.match(text[mark.end])):
                d.error(src.where, "markup", f"marked text '{surface}' splits a word")
                continue
            for i in range(mark.start, mark.end):
                covered[i] = True
            token = self._link_marked(b, linker, src, surface, mark, previous_word(text, mark.start))
            if token is not None:
                token.start, token.end = mark.start, mark.end
                tokens.append(token)
                if mark.lemma is None:
                    automatic.add(mark.start)  # {quedar}: linked by the compiler, so context may refine it
        if source not in (SRC_LESSON, SRC_DECK) and any(m.target for m in marks):
            d.warn(src.where, "markup", "{target} only makes the word bold here: cloze items come from "
                   "lesson sentences; use [word|lemma] to link without emphasis")
        all_words = [(m.start(), m.end(), m.group()) for m in WORD_RE.finditer(text)]
        free = [k for k, w in enumerate(all_words) if not covered[w[0]]]
        i = 0
        while i < len(free):
            k = free[i]
            start, end, word = all_words[k]
            matched = self._match_multiword(linker, text, all_words, k, covered)
            if matched:
                lemma, count, bounded = matched
                last_end = all_words[k + count - 1][1]
                phrase = text[start:last_end]
                if bounded:
                    tokens.append(Token(start, last_end, phrase, lemma, F.BASE, TF_MULTIWORD))
                    i += count
                    continue
                d.warn(src.where, "multiword",
                       f"'{phrase}' is inside a longer clause, so it is linked word by word; mark "
                       f"[{phrase}|{lemma.key}] if it is the expression")
            i += 1
            if word.isdigit():
                tokens.append(Token(start, end, word, None, F.BASE, TF_NUMBER))
                continue
            lemma, tag, problem, warning = linker.lookup(word, previous_word(text, start))
            if problem:
                d.error(src.where, "link", problem)
                continue
            if warning:
                d.warn(src.where, "homograph", warning)
            tokens.append(Token(start, end, word, lemma, tag, _name_flag(lemma)))
            automatic.add(start)
        tokens.sort(key=lambda t: t.start)
        self._object_pronouns(linker, text, tokens, automatic)
        self._climbing_pronouns(linker, text, tokens, automatic)
        sent = Sentence(src=src, es=text, en=nfc(src.en), note=nfc(src.note), tokens=tokens,
                        lesson=lesson, level=level, source=source)
        if any(t.flags & TF_TARGET for t in tokens):
            sent.flags |= SF_HAS_TARGET
        sent.reg = max([t.lemma.reg for t in tokens if t.lemma] or [0])
        allow = frozenset(a.lower() for a in src.allow)
        b.spanish_texts.append((src.where, text, allow))
        b.english_texts.append((src.where, sent.en))
        if sent.note:
            b.english_texts.append((src.where, sent.note))
        if not sent.en:
            d.error(src.where, "complete", "sentence has no English translation")
        sent.id = len(b.sentences)
        b.sentences.append(sent)
        return sent

    @staticmethod
    def _climbing_pronouns(linker: Linker, text: str, tokens: list[Token], automatic: set[int]) -> None:
        """Me voy a quedar, me estoy bañando: a reflexive pronoun earlier in the clause
        belongs to a bare infinitive or gerund whose verb has a reflexive twin, and the
        auxiliary it stands before is the plain verb (voy is ir, not irse)."""
        nonfinite = (F.NONFINITE["inf"], F.NONFINITE["ger"])
        for i, tok in enumerate(tokens):
            if tok.lemma is None or tok.lemma.verb is None or tok.tag not in nonfinite:
                continue
            marked = tok.start not in automatic  # {quedar|quedarse}: keep the author's lemma
            if marked:
                refl = tok.lemma if _reflexive(tok.lemma) else None
            else:
                refl = linker.reflexive_twin(tok.lemma)
            if refl is None:
                continue
            pronoun = None
            for j in range(i - 1, -1, -1):
                if any(ch in ".,;:!?¡¿—()\"“”" for ch in text[tokens[j].end:tokens[j + 1].start]):
                    break  # the clause ends here
                if tokens[j].surface.lower() in REFLEXIVE_PERSONS:
                    pronoun = j
                    break
            if pronoun is None:
                if _reflexive(tok.lemma) and not marked:
                    tok.lemma = linker.plain_twin(tok.lemma) or tok.lemma  # voy a quedar
                continue
            tok.lemma = refl
            for aux in tokens[pronoun + 1:i]:
                if aux.start in automatic and aux.lemma is not None and _reflexive(aux.lemma):
                    aux.lemma = linker.plain_twin(aux.lemma) or aux.lemma

    @staticmethod
    def _object_pronouns(linker: Linker, text: str, tokens: list[Token], automatic: set[int]) -> None:
        """la, los, las right before a conjugated verb are object pronouns (La veo, no las
        compres); anywhere else the article headword stands (la casa)."""
        for tok, nxt in zip(tokens, tokens[1:]):
            if tok.start not in automatic or tok.lemma is None or tok.lemma.pos != POS["det"]:
                continue
            if nxt.lemma is None or nxt.lemma.verb is None or not F.is_verb_tag(nxt.tag):
                continue
            if text[tok.end:nxt.start] != " ":
                continue
            pronoun = next(((lem, tag) for p, lem, tag in linker.index.get(tok.surface.lower(), [])
                            if p == 1 and lem.pos == POS["pron"]), None)
            if pronoun:
                tok.lemma, tok.tag = pronoun

    def _link_marked(self, b: Build, linker: Linker, src: SentenceSrc, surface: str,
                     mark: markup.Mark, prev: str = "") -> Token | None:
        d = self.diag
        flags = TF_TARGET if mark.target else 0
        if " " in surface:
            flags |= TF_MULTIWORD
        if mark.lemma is None:
            if " " in surface:
                words = words_of(surface)
                lemma = next((b.by_key[e.key] for e in b.course.lexicon.values()
                              if e.key in b.by_key and _auto_links(b.by_key[e.key])
                              and b.by_key[e.key].words == words), None)
                if lemma is None:
                    d.error(src.where, "link", f"no multi-word lemma '{surface}'; write [{surface}|key]")
                    return None
                return Token(0, 0, surface, lemma, F.BASE, flags)
            lemma, tag, problem, warning = linker.lookup(surface, prev)
            if problem:
                d.error(src.where, "link", problem)
                return None
            if warning:
                d.warn(src.where, "homograph", warning)
            return Token(0, 0, surface, lemma, tag, flags | _name_flag(lemma))
        lemma = b.by_key.get(nfc(mark.lemma))
        if lemma is None:
            d.error(src.where, "link", f"unknown lemma '{mark.lemma}' in markup for '{surface}'")
            return None
        possible = linker.tags_for(lemma, surface)
        if mark.form is not None:
            try:
                tag = F.parse_tag(mark.form)
            except F.TagError as exc:
                d.error(src.where, "peninsular" if "vosotros" in str(exc) else "markup", str(exc))
                return None
            expected = lemma.form_for(tag)
            if tag not in possible and not any(t & ~F.ENCLITIC == tag for t in possible):
                if expected is not None:
                    d.error(src.where, "form",
                            f"'{surface}' is not {lemma.key} {mark.form} (expected '{_bare(expected)}')")
                    return None
                if lemma.verb is not None or tag != F.BASE:
                    d.error(src.where, "form",
                            f"'{surface}' is not a known {mark.form} form of '{lemma.key}'; "
                            "add it to the lexicon 'forms' column")
                    return None
            return Token(0, 0, surface, lemma, tag, flags | _name_flag(lemma))
        if not possible:
            d.error(src.where, "form",
                    f"'{surface}' is not a known form of '{lemma.key}'; add it to the lexicon "
                    "'forms' column or fix the spelling")
            return None
        return Token(0, 0, surface, lemma, possible[0], flags | _name_flag(lemma))

    @staticmethod
    def _match_multiword(linker: Linker, text: str, words, i: int, covered):
        """(lemma, word count, bounded) for a multi-word headword starting at words[i].

        Only a match that stands alone between punctuation or the ends of the
        sentence (Gracias. —De nada.) is linked automatically; inside a longer
        clause (No sé de nada) the words may not form the expression.
        """
        first = words[i][2].lower()
        for seq, lemma in linker.multi.get(first, []):
            n = len(seq)
            if i + n > len(words) or any(covered[w[0]] for w in words[i:i + n]):
                continue
            if tuple(w[2].lower() for w in words[i:i + n]) != seq:
                continue
            # Only plain single spaces between the words: "por favor", not "por, favor".
            if not all(text[words[k][1]:words[k + 1][0]] == " " for k in range(i, i + n - 1)):
                continue
            before = text[words[i - 1][1]:words[i][0]] if i > 0 else ""
            after = text[words[i + n - 1][1]:words[i + n][0]] if i + n < len(words) else ""
            bounded = (i == 0 or before.strip() != "") and (i + n == len(words) or after.strip() != "")
            return lemma, n, bounded or lemma.pos == POS["propn"]  # Estados Unidos is never ambiguous
        return None

    # -- top level ---------------------------------------------------------------

    def run(self) -> Build:
        from . import items, review, validate  # noqa: PLC0415 (cycle-free late import)

        d = self.diag
        course = load_course(self.root, d)
        b = Build(course=course, diag=d, allow_missing_examples=self.allow_missing_examples)
        b.conjugator = self._conjugator(course)
        b.respeller = self._respeller(course)
        self._make_lemmas(b)
        for lemma in b.lemmas:
            lemma.pron = nfc(lemma.entry.pron) if lemma.entry.pron else b.respeller.text(lemma.es)
        linker = Linker(b)
        self._lessons(b, linker)
        for story in course.stories:
            self._story(b, linker, story, NONE16)
        self._phrasebook(b, linker)
        self._deck(b, linker)
        self._confusables(b)
        if b.untabled_verbs:
            names = ", ".join(sorted(b.untabled_verbs))
            d.note(None, "dictionary", f"{len(b.untabled_verbs)} dictionary verb(s) have no verb table until "
                   f"content/verbs/irregular.yaml declares them: {names}")
        self._examples(b)
        items.generate(b)
        review.check_hashes(b)
        validate.run(b)
        return b

    def _conjugator(self, course: CourseSrc) -> Conjugator:
        """irregular.yaml merged with verbs/irregular/*.yaml; a verb may be defined once."""
        merged: dict = {}
        where: dict[str, Where] = {}
        for path in course.irregular_paths:
            try:
                data = yamlio.load(path) or {}
                if not isinstance(data, dict):
                    raise ConjugationError("expected a mapping of verbs")
                Conjugator(dict(data))  # validates this file's entries
            except yamlio.YamlError as exc:
                self.diag.error(Where(path, exc.line), "yaml", str(exc).split(": ", 1)[-1])
                continue
            except ConjugationError as exc:
                self.diag.error(Where(path), "verb", str(exc))
                continue
            for verb, spec in data.items():
                loc = Where(path, getattr(spec, "line", 0) or data.line)
                if verb in merged:
                    self.diag.error(loc, "duplicate", f"verb '{verb}' is also defined at {where[verb]}")
                    continue
                merged[verb], where[verb] = spec, loc
        return Conjugator(merged)

    def _respeller(self, course: CourseSrc) -> Respeller:
        """pronunciation.yaml merged with pronunciation/*.yaml; a word may be overridden once."""
        merged: dict[str, str] = {}
        where: dict[str, str] = {}
        for path in course.pronunciation_paths:
            try:
                part = Respeller.from_file(path)
            except yamlio.YamlError as exc:
                self.diag.error(Where(exc.path, exc.line), "yaml", str(exc).split(": ", 1)[-1])
                continue
            for word, spelled in part.overrides.items():
                if word in merged:
                    self.diag.error(Where(path), "duplicate",
                                    f"the respelling of '{word}' is also given in {where[word]}")
                    continue
                merged[word], where[word] = spelled, str(Where(path))
        return Respeller(merged)

    def _lessons(self, b: Build, linker: Linker) -> None:
        d = self.diag
        introduced: dict[str, str] = {}
        for unit_index, unit in enumerate(b.course.units):
            b.english_texts.append((unit.where, unit.title_en))
            for goal in unit.goals:
                b.english_texts.append((unit.where, goal))
            b.spanish_texts.append((unit.where, unit.title, frozenset()))
            for src in unit.lessons:
                level = LEVELS.get(src.level, 0)
                if not level:
                    d.error(src.where, "complete", f"lesson level '{src.level}' is not A1, A2, B1 or B2")
                lesson = Lesson(src=src, id=len(b.lessons), unit=unit_index, level=level)
                b.lessons.append(lesson)
                b.spanish_texts.append((src.where, src.title, frozenset()))
                b.english_texts.append((src.where, src.title_en))
                for key, where in src.new:
                    lemma = b.by_key.get(nfc(key))
                    if lemma is None:
                        d.error(where, "link", f"new lemma '{key}' is not in the lexicon")
                        continue
                    if lemma.pos == POS["propn"]:
                        d.error(where, "schema", f"proper noun '{key}' cannot be a new lesson word")
                        continue
                    if key in introduced:
                        d.error(where, "duplicate", f"lemma '{key}' is already introduced in {introduced[key]}")
                        continue
                    introduced[key] = src.ref
                    lemma.lesson = lesson.id
                    lesson.new.append(lemma)
                for note_src in src.notes:
                    try:
                        spans = markup.parse_note(nfc(note_src.text))
                    except markup.MarkupError as exc:
                        d.error(note_src.where, "markup", str(exc))
                        spans = []
                    note = Note(title=nfc(note_src.title), kind=NOTE_KINDS.get(note_src.kind, 0),
                                spans=spans, lesson=lesson.id, where=note_src.where)
                    lesson.notes.append(len(b.notes))
                    b.notes.append(note)
                    b.english_texts.append((note_src.where, note.title))
                    for style, text in spans:
                        if style in markup.SPANISH_STYLES:
                            allow = frozenset({ALLOW_DIGITS} | {a.lower() for a in note_src.allow})
                            b.spanish_texts.append((note_src.where, text, allow))
                        elif style != markup.NOT_MEXICAN:
                            b.english_texts.append((note_src.where, text))
                        else:
                            b.english_texts.append((note_src.where, text))  # charset only
                for s in src.sentences:
                    sent = self._sentence(b, linker, s, lesson.id, level, SRC_LESSON)
                    if sent:
                        if s.order:
                            sent.flags |= SF_WORD_ORDER
                        lesson.sentences.append(sent.id)
                if lesson.sentences:
                    ids = lesson.sentences
                    if ids != list(range(ids[0], ids[0] + len(ids))):
                        d.error(src.where, "internal", "lesson sentences are not contiguous")
                if src.dialogue is not None:
                    lesson.dialogue = self._story(b, linker, src.dialogue, lesson.id, level)
        for story in b.course.stories:
            if story.lesson_ref and not any(l.src.ref == story.lesson_ref for l in b.lessons):
                d.error(story.where, "link", f"story lesson '{story.lesson_ref}' does not exist (use uNN.lNN)")

    def _story(self, b: Build, linker: Linker, src: StorySrc, lesson: int, level: int = 0) -> int:
        lvl = LEVELS.get(src.level, level) if src.level else level
        if src.level and src.level not in LEVELS:
            self.diag.error(src.where, "complete", f"story level '{src.level}' is not A1, A2, B1 or B2")
        if src.lesson_ref:
            lesson = next((l.id for l in b.lessons if l.src.ref == src.lesson_ref), lesson)
        lines = []
        source = SRC_DIALOGUE if lesson != NONE16 and src.kind == "dialogue" else SRC_STORY
        for line in src.lines:
            sent = self._sentence(b, linker, line, lesson, lvl or 1, source)
            if sent:
                lines.append((nfc(line.speaker), sent.id, 1 if line.paragraph else 0))
                if line.speaker:
                    b.english_texts.append((line.where, line.speaker))
        questions = []
        for q in src.questions:
            where = q["where"]
            options = [nfc(str(o)) for o in (q.get("options") or [])]
            lang = str(q.get("lang", "es"))
            answer = q.get("answer", 0)
            if not 2 <= len(options) <= 4:
                self.diag.error(where, "schema", "a question needs 2 to 4 options")
            if not isinstance(answer, int) or not 0 <= answer < len(options):
                self.diag.error(where, "schema", f"answer {answer!r} is not an option index (0-based)")
                answer = 0
            if lang not in ("es", "en"):
                self.diag.error(where, "schema", "question lang must be es or en")
            text = nfc(str(q["q"]))
            for t in [text] + options:
                if lang == "es":
                    b.spanish_texts.append((where, t, frozenset()))
                else:
                    b.english_texts.append((where, t))
            questions.append({"text": text, "options": options, "answer": answer, "spanish": lang == "es",
                              "where": where})
        b.spanish_texts.append((src.where, src.title, frozenset()))
        b.english_texts.append((src.where, src.title_en))
        b.stories.append(Story(title=nfc(src.title), title_en=nfc(src.title_en),
                               kind=STORY_KINDS.get(src.kind, 0), level=lvl or 1, lesson=lesson,
                               lines=lines, questions=questions, src=src))
        return len(b.stories) - 1

    def _phrasebook(self, b: Build, linker: Linker) -> None:
        for cat_index, cat in enumerate(b.course.phrasebook):
            first = len(b.phrase_entries)
            for phrase in cat.phrases:
                sent = self._sentence(b, linker, phrase, NONE16, 1, SRC_PHRASE)
                if sent:
                    sent.level = max([t.lemma.level for t in sent.tokens if t.lemma] or [1])
                    pron = nfc(phrase.pron) if phrase.pron else b.respeller.text(sent.es)
                    b.phrase_entries.append(PhraseEntry(sentence=sent.id, category=cat_index, pron=pron))
            b.phrase_categories.append({"title": nfc(cat.title), "title_en": nfc(cat.title_en),
                                        "first": first, "count": len(b.phrase_entries) - first,
                                        "src": cat})
            b.spanish_texts.append((cat.where, cat.title, frozenset()))
            b.english_texts.append((cat.where, cat.title_en))

    def _deck(self, b: Build, linker: Linker) -> None:
        """content/deck/*.yaml: example and cloze sentences for frequency-deck words."""
        for src in b.course.deck:
            ids = []
            for s in src.sentences:
                sent = self._sentence(b, linker, s, NONE16, 1, SRC_DECK)
                if sent:
                    sent.level = max([t.lemma.level for t in sent.tokens if t.lemma] or [1])
                    if s.order:
                        sent.flags |= SF_WORD_ORDER
                    ids.append(sent.id)
            b.deck_files.append({"src": src, "sentences": ids})
            b.spanish_texts.append((src.where, src.title, frozenset()))

    def _confusables(self, b: Build) -> None:
        for conf in b.course.confusables:
            members = []
            for key in conf.lemmas:
                lemma = b.by_key.get(nfc(key))
                if lemma is None:
                    self.diag.error(conf.where, "link", f"confusable lemma '{key}' is not in the lexicon")
                    continue
                lemma.flags |= LF_CONFUSABLE
                members.append(lemma)
            if not 2 <= len(members) <= 6:
                self.diag.error(conf.where, "schema", "a confusable set needs 2 to 6 lemmas")
            b.confusables.append({"name": nfc(conf.name), "note": nfc(conf.note), "members": members[:6]})
            b.english_texts.append((conf.where, conf.note))

    def _examples(self, b: Build) -> None:
        for sent in b.sentences:
            for tok in sent.tokens:
                if tok.lemma is None:
                    continue
                lst = tok.lemma.target_examples if tok.flags & TF_TARGET else tok.lemma.examples
                if sent.id not in lst:
                    lst.append(sent.id)
        for lemma in b.lemmas:
            ordered = lemma.target_examples + [s for s in lemma.examples if s not in lemma.target_examples]
            lemma.examples = ordered[:8]


def _name_flag(lemma: Lemma | None) -> int:
    return TF_NAME if lemma is not None and lemma.pos == POS["propn"] else 0


def _takes_el(noun: str) -> bool:
    """Feminine nouns beginning with a stressed a/ha take el in the singular (el agua)."""
    word = noun.split(" ")[0].lower()
    letters = word[1:] if word.startswith("h") else word
    if not letters or phon.strip_accents(letters[0]) != "a":
        return False
    syllables = phon.syllabify(word)
    return phon.stress_index(word, syllables) == 0


_EN_STOP = frozenset("a an the to of in on at for with and or be is are do does my your his her its "
                     "our their it you i me he she we they by from as".split())


def english_senses(gloss: str) -> set[str]:
    senses = set()
    for sense in re.split(r"[;,]", re.sub(r"\([^)]*\)", "", gloss)):
        key = fold(sense)
        for article in ("to ", "a ", "an ", "the "):
            if key.startswith(article):
                key = key[len(article):]
        if key:
            senses.add(key)
    return senses


def english_keys(gloss: str) -> list[tuple[str, int]]:
    """EKEY entries for a gloss: (key, rank)."""
    out: dict[str, int] = {}
    for sense in english_senses(gloss):
        out[sense] = 0
        words = sense.split(" ")
        if len(words) > 1:
            for w in words:
                if len(w) > 1 and w not in _EN_STOP and w not in out:
                    out[w] = 1
    return sorted(out.items())


def compile_course(content_root: str, diag: Diagnostics | None = None,
                   allow_missing_examples: bool = False) -> Build:
    return Compiler(os.path.abspath(content_root), diag, allow_missing_examples).run()

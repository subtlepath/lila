"""Validators of PLAN.md 7.2 step 5 that are not already enforced while linking."""

from __future__ import annotations

import os
import sys

from .build import ALLOW_DIGITS, NONE16, WORD_RE, Build
from .fold import fold
from .sources import POS

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import charset  # noqa: E402  (tools/charset.py, shared with the font converter)

# Character budgets standing in for real font metrics (PLAN.md 5.2), which are
# not available to the compiler yet. Widths assume the narrower 480 px screen
# less 24 px margins (432 px) and typical advances of the BDF strikes:
# Times Bold 34 px ~18 px/char, Times 29 px ~13, Helvetica 25 px ~13,
# Helvetica 20 px ~10. Replace with measured widths once src/fonts is final.
BUDGET = {
    "headword": 24,      # one line of Times Bold 34 px, the smallest headword strike
    "gloss": 66,         # two lines of Helvetica 25 px on a card
    "respelling": 40,    # one line of Helvetica Oblique 20 px
    "sentence": 132,     # four lines of Times 29 px under a card headword
    "translation": 172,  # four lines of Helvetica 20 px
    "line": 300,         # one reader/dialogue sentence: the reader paginates, but a sentence fits a page
    "word": 22,          # the longest word must fit one line at text size L
    "title": 30,         # header line, Helvetica Bold 25 px
    "note": 1800,        # about three pages of Helvetica 20 px
    "question": 132,
    "option": 40,
    "phrase": 90,
}

# Peninsular forms (PLAN.md 7.2). Words a sentence may still need in a Mexican
# sense (piso = floor, vale = it is worth) are allowed per sentence with `allow:`.
PENINSULAR = {
    "vosotros": "Mexico uses ustedes", "vosotras": "Mexico uses ustedes",
    "os": "vosotros pronoun; Mexico uses les/los/se", "vuestro": "Mexico uses su/de ustedes",
    "vuestra": "Mexico uses su/de ustedes", "vuestros": "Mexico uses sus/de ustedes",
    "vuestras": "Mexico uses sus/de ustedes",
    "ordenador": "Mexico says computadora", "móvil": "Mexico says celular", "zumo": "Mexico says jugo",
    "patata": "Mexico says papa", "patatas": "Mexico says papas",
    "piso": "for an apartment Mexico says departamento (allow it for 'floor')",
    "vale": "as 'OK' Mexico says sale/está bien/ok (allow it for 'vale la pena')",
    "gafas": "Mexico says lentes", "aparcar": "Mexico says estacionar", "coger": "vulgar in Mexico; use tomar or agarrar",
    "chaqueta": "vulgar slang in Mexico; Mexico says chamarra", "guay": "Mexico says padre/chido",
    "melocotón": "Mexico says durazno", "judías": "Mexico says frijoles",
}
PENINSULAR_ENDING_EXCEPTIONS = frozenset({"veintiséis", "dieciséis"})
# Peninsular only in one sense: a lexicon entry for the Mexican sense is fine,
# and each sentence that uses it says so with `allow:`.
PENINSULAR_IN_ONE_SENSE = frozenset({"piso", "vale"})

# Words that must carry at least this register (PLAN.md 4.2). pos narrows a
# word that also has a neutral sense (padre = father, fresa = strawberry).
MIN_REGISTER = {
    ("órale", None): 2, ("ándale", None): 2, ("híjole", None): 2, ("qué onda", None): 2,
    ("chido", None): 2, ("padre", "adj"): 2, ("chamba", None): 2, ("neta", None): 2,
    ("no manches", None): 2, ("güey", None): 2, ("wey", None): 2, ("cuate", None): 2,
    ("chela", None): 2, ("naco", None): 2, ("fresa", "adj"): 2, ("chafa", None): 2, ("gacho", None): 2,
    ("carnal", "noun"): 2, ("morro", None): 2, ("morra", None): 2, ("chavo", None): 2, ("chava", None): 2,
    ("aguas", "interj"): 2, ("simón", None): 2, ("nel", None): 2, ("qué padre", None): 2,
    ("cruda", "noun"): 2, ("pedo", None): 2, ("lana", "money"): 2,
    ("pinche", None): 3, ("chingar", None): 3, ("chingón", None): 3, ("chingada", None): 3,
    ("pendejo", None): 3, ("cabrón", None): 3, ("culero", None): 3, ("verga", None): 3,
    ("coger", None): 3, ("madrazo", None): 3, ("puto", None): 3, ("mamón", None): 3,
}


def _over(b: Build, kind: str, text: str, where, what: str) -> None:
    limit = BUDGET[kind]
    if len(text) > limit:
        b.diag.error(where, "fit", f"{what} is {len(text)} characters; the {kind} budget is {limit}: {text[:60]}")


def run(b: Build) -> None:
    d = b.diag
    _charset(b)
    _lint(b)

    for lemma in b.lemmas:
        where = lemma.entry.where
        _over(b, "headword", lemma.es, where, "headword")
        _over(b, "gloss", lemma.en, where, "gloss")
        _over(b, "respelling", lemma.pron, where, "respelling")
        if not lemma.pron:
            d.error(where, "complete", f"lemma '{lemma.es}' has no respelling")
        if not lemma.examples and lemma.pos != POS["propn"] and not lemma.entry.dictionary:
            report = d.warn if b.allow_missing_examples else d.error
            report(where, "example", f"lemma '{lemma.key}' has no example sentence")
        if lemma.entry.pos != "noun" and lemma.entry.gender and lemma.entry.pos not in ("propn", "pron", "det"):
            d.error(where, "schema", f"gender is only for nouns, pronouns and determiners ('{lemma.key}')")
        required = _min_register(lemma)
        if required and lemma.reg < required:
            name = ("informal", "vulgar")[required - 2]
            d.error(where, "register", f"'{lemma.es}' must be tagged register {name} or stronger")

    for sent in b.sentences:
        where = sent.src.where
        long_kind = "line" if sent.source in (1, 2) else "phrase" if sent.source == 3 else "sentence"
        _over(b, long_kind, sent.es, where, "Spanish")
        _over(b, "translation" if long_kind != "line" else "line", sent.en, where, "English")
        for tok in sent.tokens:
            if len(tok.surface) > BUDGET["word"] and not (tok.flags & 2):
                _over(b, "word", tok.surface, where, "word")
        if len(sent.tokens) > 255:
            d.error(where, "fit", "more than 255 words in one sentence")

    for lesson in b.lessons:
        _over(b, "title", lesson.src.title, lesson.src.where, "lesson title")
        if not lesson.new:
            d.warn(lesson.src.where, "complete", "lesson introduces no new lemmas")
        if not lesson.sentences:
            d.error(lesson.src.where, "complete", "lesson has no sentences")
    for unit in b.course.units:
        _over(b, "title", unit.title, unit.where, "unit title")
    for note in b.notes:
        size = sum(len(t) for _s, t in note.spans)
        _over(b, "note", "x" * size, note.where, "note")
    for story in b.stories:
        for q in story.questions:
            _over(b, "question", q["text"], q["where"], "question")
            for option in q["options"]:
                _over(b, "option", option, q["where"], "option")

    counts = {"lemmas": len(b.lemmas), "sentences": len(b.sentences), "items": len(b.items),
              "lessons": len(b.lessons), "stories": len(b.stories), "verb tables": len(b.verbs)}
    for what, n in counts.items():
        if n >= NONE16:
            d.error(None, "fit", f"{n} {what}: the pack format allows at most 65,534")


def _min_register(lemma) -> int:
    key = fold(lemma.es)
    best = 0
    for (word, qualifier), level in MIN_REGISTER.items():
        if fold(word) != key:
            continue
        if qualifier is None or qualifier == lemma.entry.pos or (
                qualifier == "money" and "money" in lemma.en.lower()):
            best = max(best, level)
    return best


def _charset(b: Build) -> None:
    d = b.diag
    texts = [(w, t) for w, t, _a in b.spanish_texts] + list(b.english_texts)
    for lemma in b.lemmas:
        e = lemma.entry
        texts += [(e.where, lemma.es), (e.where, lemma.en), (e.where, lemma.pron), (e.where, e.note),
                  (e.where, e.alt)]
    for entry in b.phrase_entries:
        texts.append((b.sentences[entry.sentence].src.where, entry.pron))
    for story in b.stories:
        for speaker, _sid, _f in story.lines:
            texts.append((story.src.where, speaker))
    reported = set()
    for where, text in texts:
        bad = charset.unsupported(text or "")
        if bad and (str(where), tuple(bad)) not in reported:
            reported.add((str(where), tuple(bad)))
            shown = ", ".join(f"{ch!r} (U+{ord(ch):04X})" for ch in bad)
            d.error(where, "charset", f"character(s) not in the font charset: {shown} in {text[:60]!r}")


def _lint(b: Build) -> None:
    d = b.diag
    texts = list(b.spanish_texts)
    for lemma in b.lemmas:
        if lemma.entry.dictionary and lemma.entry.note.strip():
            continue  # a dictionary entry may define coger or ordenador to warn the learner (its note)
        texts.append((lemma.entry.where, lemma.es, PENINSULAR_IN_ONE_SENSE))
    for where, text, allow in texts:
        for word in WORD_RE.findall(text or ""):
            low = word.lower()
            if low in allow:
                continue
            if low in PENINSULAR:
                d.error(where, "peninsular", f"'{word}' is Peninsular: {PENINSULAR[low]}")
            elif (low.endswith("áis") or low.endswith("éis")) and low not in PENINSULAR_ENDING_EXCEPTIONS:
                d.error(where, "peninsular", f"'{word}' is a vosotros verb form; Mexico uses ustedes")
            elif any(ch.isdigit() for ch in word) and ALLOW_DIGITS not in allow:
                # Digits teach nothing about the Spanish number and the respelling cannot read them.
                d.error(where, "digits", f"spell out the number '{word}' in Spanish (e.g. veinte), "
                        f"or exempt it with allow: [{word}]")

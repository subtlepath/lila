"""Form tags (docs/pack-format.md 3.4) and noun/adjective inflection."""

from __future__ import annotations

from . import phon

TENSES = ("pres", "pret", "impf", "fut", "cond", "subj", "imp", "impneg")
PERSONS = ("1s", "2s", "3s", "1p", "3p")
IMPERATIVE_PERSONS = ("2s", "3s", "3p")

TENSE_NAMES = {
    "pres": "present", "pret": "preterite", "impf": "imperfect", "fut": "future",
    "cond": "conditional", "subj": "present subjunctive", "imp": "imperative",
    "impneg": "negative imperative",
}
PERSON_NAMES = {"1s": "yo", "2s": "tú", "3s": "él/ella/usted", "1p": "nosotros", "3p": "ellos/ustedes"}

BASE = 0x0000
ENCLITIC = 0x0100
NONFINITE = {"inf": 0x2000, "ger": 0x2001, "part": 0x2002, "part.f": 0x2003, "part.mpl": 0x2004,
             "part.fpl": 0x2005}
NOMINAL = {"sg": 0x3000, "pl": 0x3001, "m": 0x3010, "mpl": 0x3011, "f": 0x3020, "fpl": 0x3021}
APOCOPE = 0x4001
OTHER = {"base": BASE, "apoc": APOCOPE}

_NAME_BY_TAG = {**{v: k for k, v in NONFINITE.items()}, **{v: k for k, v in NOMINAL.items()},
                BASE: "base", APOCOPE: "apoc"}


class TagError(ValueError):
    pass


def verb_tag(tense: str, person: str) -> int:
    return 0x1000 | (TENSES.index(tense) << 4) | PERSONS.index(person)


def parse_tag(text: str) -> int:
    """`pres.3s` -> 0x1002. Raises TagError, with a vosotros-specific message for 2p."""
    text = text.strip()
    if text in NONFINITE:
        return NONFINITE[text]
    if text in NOMINAL:
        return NOMINAL[text]
    if text in OTHER:
        return OTHER[text]
    tense, _, person = text.partition(".")
    if tense in TENSES:
        if person in ("2p", "2pl"):
            raise TagError(f"form '{text}' is a vosotros form; Mexican Spanish uses ustedes (3p)")
        if person not in PERSONS:
            raise TagError(f"unknown person in form '{text}' (use 1s 2s 3s 1p 3p)")
        if tense in ("imp", "impneg") and person not in IMPERATIVE_PERSONS:
            raise TagError(f"no imperative for person {person} in '{text}' (use 2s 3s 3p)")
        return verb_tag(tense, person)
    raise TagError(f"unknown form tag '{text}'")


def tag_name(tag: int) -> str:
    if tag in _NAME_BY_TAG:
        return _NAME_BY_TAG[tag]
    if tag & 0xF000 == 0x1000:
        name = f"{TENSES[(tag >> 4) & 0xF]}.{PERSONS[tag & 0xF]}"
        return name + "+clitic" if tag & ENCLITIC else name
    return f"0x{tag:04x}"


def is_verb_tag(tag: int) -> bool:
    return tag & 0xF000 == 0x1000


def verb_tag_parts(tag: int) -> tuple[str, str]:
    return TENSES[(tag >> 4) & 0xF], PERSONS[tag & 0xF]


# --- nominal inflection -----------------------------------------------------

# Plurals that move the stress (régimen -> regímenes) are not rule-made.
IRREGULAR_PLURALS = {"régimen": "regímenes", "carácter": "caracteres", "espécimen": "especímenes"}

_INVARIABLE_OR = frozenset({"mejor", "peor", "mayor", "menor", "superior", "inferior", "interior",
                            "exterior", "anterior", "posterior", "ulterior"})


def pluralize(word: str) -> str:
    """Regular plural of a single noun or adjective (multi-word: first word only)."""
    if " " in word:
        head, _, rest = word.partition(" ")
        return pluralize(head) + " " + rest
    if not word:
        return word
    if word in IRREGULAR_PLURALS:
        return IRREGULAR_PLURALS[word]
    last = word[-1]
    syllables = phon.syllabify(word)
    stressed = phon.stress_index(word, syllables)
    if last in "aeiouáéó":
        return word + "s"
    if last in "íú":
        return word + "es"
    if last in "sx":
        if len(syllables) > 1 and stressed < len(syllables) - 1:
            return word  # lunes, crisis, tórax
        stem = word
    elif last == "z":
        stem = word[:-1] + "c"
    else:
        stem = word
    # place_accent drops accents the plural no longer needs but keeps one that
    # marks a hiatus: país -> países, baúl -> baúles, camión -> camiones.
    plural = stem + "es"
    return phon.place_accent(phon.syllabify(plural), stressed)


def feminine(word: str) -> str | None:
    """Feminine singular of an adjective or person noun, or None if invariable."""
    if " " in word:
        head, _, rest = word.partition(" ")
        fem = feminine(head)
        return None if fem is None else fem + " " + rest
    if word.endswith("o"):
        return word[:-1] + "a"
    if word in _INVARIABLE_OR:
        return None
    if word.endswith("or"):
        return word + "a"
    for ending in ("ón", "án", "ín", "és"):
        if word.endswith(ending):
            return word[:-2] + phon.strip_accents(ending) + "a"  # inglés -> inglesa
    return None

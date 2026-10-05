"""Spanish written syllables and stress.

Shared by the conjugator (accents on forms with attached pronouns), the
respeller (syllable boundaries and the stressed syllable) and nominal
inflection (`joven` -> `jóvenes`). Works on single lower-case words.
"""

from __future__ import annotations

ACUTE = {"a": "á", "e": "é", "i": "í", "o": "ó", "u": "ú"}
PLAIN = {v: k for k, v in ACUTE.items()}
VOWELS = frozenset("aeiouáéíóúü")
_OPEN = frozenset("aeoáéó")       # a, e, o
_STRESSED_CLOSE = frozenset("íú")  # an accented i/u always forms a hiatus with a, e, o
_CLOSE = frozenset("iuü")

# Consonant pairs that always begin a syllable together. `tl` follows the
# Mexican convention (a-tle-ta, Tlá-loc).
INSEPARABLE = frozenset(
    {"bl", "br", "cl", "cr", "dr", "fl", "fr", "gl", "gr", "kl", "kr", "pl", "pr", "tl", "tr"}
)


def _units(word: str) -> list[tuple[str, bool]]:
    """Split a word into (letters, is_vowel) units: digraphs and silent u are consonant units."""
    units: list[tuple[str, bool]] = []
    i = 0
    n = len(word)
    while i < n:
        ch = word[i]
        two = word[i:i + 2]
        nxt = word[i + 2:i + 3]
        if two in ("ch", "ll", "rr"):
            units.append((two, False))
            i += 2
        elif two in ("qu", "gu") and nxt in ("e", "i", "é", "í"):
            units.append((two, False))
            i += 2
        elif ch == "y":
            # A y that is not followed by a vowel is the vowel i (hoy, muy, y).
            before_vowel = i + 1 < n and word[i + 1] in VOWELS
            units.append((ch, not before_vowel))
            i += 1
        else:
            units.append((ch, ch in VOWELS))
            i += 1
    return units


def _hiatus(a: str, b: str) -> bool:
    a = "i" if a == "y" else a
    b = "i" if b == "y" else b
    if a in _OPEN and b in _OPEN:
        return True
    if (a in _STRESSED_CLOSE and b in _OPEN) or (a in _OPEN and b in _STRESSED_CLOSE):
        return True
    return PLAIN.get(a, a) == PLAIN.get(b, b)  # chi-i-ta and also fri-í-si-mo


def syllabify(word: str) -> list[str]:
    """Written syllables of one lower-case word: gracias -> [gra, cias], día -> [dí, a]."""
    units = _units(word)
    # Nuclei: runs of vowel units, split at each hiatus.
    nuclei: list[tuple[int, int]] = []  # [start, end) unit indexes
    i = 0
    while i < len(units):
        if not units[i][1]:
            i += 1
            continue
        start = i
        i += 1
        while i < len(units) and units[i][1] and not _hiatus(units[i - 1][0], units[i][0]):
            i += 1
        nuclei.append((start, i))
        # A hiatus starts a new nucleus at i (still a vowel); the loop picks it up.
    if not nuclei:
        return [word]

    cuts: list[int] = []  # unit index where each syllable after the first begins
    for (_, end), (start, _) in zip(nuclei, nuclei[1:]):
        cons = [u for u, _v in units[end:start]]
        k = len(cons)
        if k == 0:
            cut = start
        elif k == 1:
            cut = end
        elif k == 2:
            cut = end if cons[0] + cons[1] in INSEPARABLE else end + 1
        elif k == 3:
            cut = end + 1 if cons[1] + cons[2] in INSEPARABLE else end + 2
        else:
            cut = start - 2 if cons[-2] + cons[-1] in INSEPARABLE else start - 1
        cuts.append(cut)

    syllables: list[str] = []
    prev = 0
    for cut in cuts + [len(units)]:
        syllables.append("".join(u for u, _v in units[prev:cut]))
        prev = cut
    return syllables


def default_stress(word: str, count: int) -> int:
    """Stressed syllable index given by the rules for a word with no written accent."""
    if count <= 1:
        return 0
    last = word[-1:]
    if last in "aeiou" or last in "ns":
        return count - 2
    return count - 1


def stress_index(word: str, syllables: list[str] | None = None) -> int:
    """Index of the stressed syllable of `word`."""
    syllables = syllables if syllables is not None else syllabify(word)
    for idx in range(len(syllables) - 1, -1, -1):
        if any(ch in PLAIN for ch in syllables[idx]):
            return idx
    return default_stress(word, len(syllables))


def strip_accents(word: str) -> str:
    """Remove acute accents (keeps ñ and ü)."""
    return "".join(PLAIN.get(ch, ch) for ch in word)


def has_accent(word: str) -> bool:
    return any(ch in PLAIN for ch in word)


def _accent_syllable(syllable: str) -> str:
    vowels = [i for i, ch in enumerate(syllable) if ch in "aeiou"]
    if not vowels:
        return syllable
    # The open vowel carries the accent in a diphthong; in iu/ui the second does.
    target = next((i for i in vowels if syllable[i] in "aeo"), vowels[-1])
    return syllable[:target] + ACUTE[syllable[target]] + syllable[target + 1:]


def place_accent(syllables: list[str], stressed: int) -> str:
    """Join syllables, writing an accent only where the rules need one for `stressed`.

    A written accent that marks a hiatus (`oír`, `reírse`) is kept when removing
    it would merge the syllables.
    """
    plain = [strip_accents(s) for s in syllables]
    word = "".join(plain)
    if len(syllables) == 1:
        return word
    if syllabify(word) == plain and default_stress(word, len(plain)) == stressed:
        return word
    out = list(plain)
    out[stressed] = _accent_syllable(plain[stressed])
    return "".join(out)


def attach(form: str, *clitics: str) -> str:
    """Attach unstressed pronouns to a verb form: llama + te -> llámate, diga + me -> dígame."""
    syllables = syllabify(form)
    stressed = stress_index(form, syllables)
    extra: list[str] = []
    for clitic in clitics:
        extra += syllabify(clitic)
    return place_accent(syllables + extra, stressed)

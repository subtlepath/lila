"""Mexican Spanish respelling for English speakers (PLAN.md 4.2, 7.2 step 3).

Every lemma and phrase in the pack carries a respelling that an English
speaker can read aloud without knowing IPA (the Times and Helvetica strikes
lack IPA letters). This docstring is the style guide that
`content/pronunciation.yaml` and hand-written `pron:` fields follow.

Form
----
- Plain ASCII letters, hyphens and spaces only.
- Syllables are joined by `-`; words are separated by one space.
- The stressed syllable is in CAPITALS, every other syllable lower case:
  `aguacate` -> `ah-gwah-KAH-teh`.
- A word respelled on its own is always stressed, so a monosyllable alone is
  all capitals: `sol` -> `SOHL`, `sí` -> `SEE`, `de` -> `DEH`.
- In a phrase the unstressed function words stay lower case (`el la los las lo
  le les de del al a en con por sin y e o u ni que si me te se nos mi mis tu
  tus su sus un tras`); every other word keeps its stress:
  `por favor` -> `pohr fah-BOHR`, `¿qué onda?` -> `KEH OHN-dah`. Accented
  forms (`él tú mí sé té dé sí qué`) are stressed words.
- Punctuation (`¿ ? ¡ ! , . : ; — “ ” « »`) and digits are dropped.

Vowels and diphthongs
---------------------
| Spanish               | Respelling | Example                          |
|-----------------------|------------|----------------------------------|
| a e i o u             | ah eh ee oh oo | `hola` OH-lah, `tú` TOO      |
| ia ie io iu           | yah yeh yoh yoo | `gracias` GRAH-syahs, `ciudad` syoo-DAHD |
| ua ue uo ui (uy)      | wah weh woh wee | `cuate` KWAH-teh, `muy` MWEE |
| ai ay                 | eye; ye after a consonant | `hay` EYE, `baile` BYE-leh |
| ei ey                 | ay         | `rey` RRAY, `veinte` BAYN-teh    |
| oi oy                 | oy         | `hoy` OY, `estoy` ehs-TOY        |
| au                    | ow         | `auto` OW-toh                    |
| eu, ou                | eh-oo, oh-oo (only the first part capitalised) | `deuda` DEH-oo-dah |
| uai uay, uei uey      | wye, way   | `Uruguay` oo-roo-GWYE, `buey` BWAY |
| iau, uau              | yow, wow   | `miau` MYOW                      |

Two vowels in hiatus stay separate syllables: `día` DEE-ah, `país` pah-EES,
`leer` leh-EHR. The conjunction `y` and a final `y` are vowels (`y` ee).

Consonants
----------
| Spanish                          | Respelling | Example                       |
|----------------------------------|------------|-------------------------------|
| b, v                             | b          | `llave` YAH-beh               |
| c before e/i, z, s (seseo)       | s          | `cerveza` sehr-BEH-sah        |
| c, k, qu                         | k          | `queso` KEH-soh               |
| ch                               | ch         | `chamba` CHAHM-bah            |
| g before e/i, j                  | h          | `gente` HEHN-teh, `jitomate` hee-toh-MAH-teh |
| g, gu before e/i                 | g          | `guitarra` gee-TAH-rrah       |
| gü before e/i                    | gw         | `pingüino` peen-GWEE-noh      |
| h                                | silent; hie/hia = y, hue/hua/hui = w | `hielo` YEH-loh, `huevo` WEH-boh |
| ll, consonant y (yeísmo)         | y          | `calle` KAH-yeh, `yo` YOH     |
| ñ                                | ny         | `niño` NEE-nyoh               |
| rr; r at word start or after n, l, s | rr     | `perro` PEH-rroh, `honra` OHN-rrah |
| r elsewhere                      | r          | `pero` PEH-roh                |
| x                                | ks (k ends the syllable before: `taxi` TAHK-see); s at word start | `examen` ehk-SAH-mehn |
| tl                               | tl, kept together | `atleta` ah-TLEH-tah   |
| d, f, l, m, n, p, t, w           | unchanged  | `usted` oos-TEHD              |

Syllable boundaries follow the written ones (`tools/packc/phon.py`) except
where the sound needs otherwise: `x` between vowels splits into k|s, and an
`s` sound that ends a syllable merges into an `s` that begins the next
(`piscina` pee-SEE-nah, `excelente` ehk-seh-LEHN-teh). A doubled consonant
inside one syllable is written once (`rock` RROHK), which also makes a final
`j` after a, e or o silent, as most Mexicans say it (`reloj` rreh-LOH). `sh`
is not produced by rule: loans and names that need it are overrides.

Overrides
---------
The other readings of `x` (`México` MEH-hee-koh, `Xola` SHOH-lah, `Tlaxcala`
tlahs-KAH-lah), place names and English loans (`pizza` PEET-sah, `jeans`
YEENS) go in `content/pronunciation.yaml` under `overrides:`, keyed by the
lower-case word with its accents. A key may be several words (`hot dog`). An
override is used verbatim for that exact word form, so list inflected forms
separately (`mexicano`, `mexicana`, `mexicanos`). A lemma or phrase can also
carry its own `pron:`, which skips the respeller entirely.
"""

from __future__ import annotations

import re
import unicodedata

from . import phon, yamlio

# What a respelling may contain: ASCII words joined by single hyphens or spaces.
RESPELLING_RE = re.compile(r"^[A-Za-z]+(?:[- ][A-Za-z]+)*$")

FUNCTION_WORDS = frozenset(
    "el la los las lo le les de del al a en con por sin y e o u ni que si "
    "me te se nos mi mis tu tus su sus un tras".split()
)

_WORD_RE = re.compile(r"[^\W\d_]+(?:['’-][^\W\d_]+)*")
_SPANISH_LETTERS = frozenset("abcdefghijklmnopqrstuvwxyzáéíóúüñ")
_VOWELS = frozenset("aeiouáéíóúü")
_FRONT = frozenset("eéií")
_BASE_VOWEL = {"á": "a", "é": "e", "í": "i", "ó": "o", "ú": "u", "ü": "u", "y": "i"}
_CONSONANT = {"b": "b", "d": "d", "f": "f", "j": "h", "k": "k", "l": "l", "m": "m", "n": "n",
              "ñ": "ny", "p": "p", "q": "k", "s": "s", "t": "t", "v": "b", "w": "w", "z": "s"}

_NUCLEUS = {
    "a": "ah", "e": "eh", "i": "ee", "o": "oh", "u": "oo",
    "ia": "yah", "ie": "yeh", "io": "yoh", "iu": "yoo",
    "ua": "wah", "ue": "weh", "uo": "woh", "ui": "wee",
    "ai": "eye", "ei": "ay", "oi": "oy", "au": "ow", "eu": "eh-oo", "ou": "oh-oo",
    "uai": "wye", "uei": "way", "iai": "yeye", "iei": "yay", "iau": "yow", "uau": "wow",
    "ioi": "yoy", "uoi": "woy",
}


def _norm(text: str) -> str:
    return unicodedata.normalize("NFC", text).lower()


def _tokens(text: str) -> list[str]:
    return _WORD_RE.findall(_norm(text))


def _letters(token: str) -> str:
    """Letters of a token, with letters outside Spanish folded to their ASCII base (ç -> c)."""
    out = []
    for ch in token:
        if ch in _SPANISH_LETTERS:
            out.append(ch)
        elif ch.isalpha():
            base = unicodedata.normalize("NFD", ch)[0]
            if base.isascii() and base.isalpha():
                out.append(base)
    return "".join(out)


def _cap(syllable: str) -> str:
    """Capitalise a stressed syllable; in eh-oo/oh-oo only the first part carries the stress."""
    head, sep, tail = syllable.partition("-")
    return head.upper() + sep + tail


def _nucleus(vowels: str, after_consonant: bool) -> str:
    spelled = _NUCLEUS.get(vowels)
    if spelled is None:  # not a Spanish diphthong; phon splits these, so this is a fallback
        spelled = "".join(_NUCLEUS.get(v, v) for v in vowels)
    if after_consonant and spelled.startswith("eye"):
        spelled = spelled[1:]  # baile -> BYE-leh, not BEYE-leh
    return spelled


def _sounds(word: str, syllables: list[str]) -> list[tuple[list[str], str, list[str]]]:
    """(onset sounds, nucleus vowels, coda sounds) for each written syllable."""
    owner: list[int] = []
    for index, syllable in enumerate(syllables):
        owner += [index] * len(syllable)
    parts: list[tuple[list[str], list[str], list[str]]] = [([], [], []) for _ in syllables]
    n = len(word)
    i = 0
    while i < n:
        ch = word[i]
        prev = word[i - 1] if i else ""
        nxt = word[i + 1] if i + 1 < n else ""
        onset, nucleus, coda = parts[owner[i]]
        step = 1
        sound = ""
        if ch in _VOWELS or (ch == "y" and nxt not in _VOWELS):
            if not (ch == "u" and prev in ("q", "g") and nxt in _FRONT):  # silent u in que, gui
                nucleus.append(_BASE_VOWEL.get(ch, ch))
            i += 1
            continue
        if ch == "c":
            if nxt == "h":
                sound, step = "ch", 2
            else:
                sound = "s" if nxt in _FRONT else "k"
        elif ch == "l" and nxt == "l":
            sound, step = "y", 2
        elif ch == "r":
            if nxt == "r":
                sound, step = "rr", 2
            else:
                sound = "rr" if i == 0 or prev in ("n", "l", "s") else "r"
        elif ch == "g":
            sound = "h" if nxt in _FRONT else "g"
        elif ch == "x":
            sound = "s" if i == 0 else "ks"
        elif ch == "y":
            sound = "y"
        elif ch == "h":
            sound = ""  # silent; ch is handled with c
        else:
            sound = _CONSONANT.get(ch, ch)
        if sound:
            (coda if nucleus else onset).append(sound)
        i += step
    return [(onset, "".join(nucleus), coda) for onset, nucleus, coda in parts]


def _respell(word: str) -> str:
    """Respelling of one bare lower-case word by rule, stressed syllable in capitals."""
    syllables = phon.syllabify(word)
    stressed = phon.stress_index(word, syllables)
    parts = _sounds(word, syllables)

    for k in range(1, len(parts)):
        onset, _, _ = parts[k]
        _, prev_nucleus, prev_coda = parts[k - 1]
        # taxi: ta-xi is said tak-si.
        if onset and onset[0] == "ks" and prev_nucleus and not prev_coda:
            prev_coda.append("k")
            onset[0] = "s"
        # piscina, excelente: one s sound across the boundary.
        if onset and onset[0].startswith("s") and prev_coda and prev_coda[-1].endswith("s"):
            prev_coda[-1] = prev_coda[-1][:-1]

    out = []
    for onset, vowels, coda in parts:
        head = "".join(onset)
        text = head + (_nucleus(vowels, bool(head)) if vowels else "") + "".join(coda)
        out.append(re.sub(r"([b-df-hj-np-qs-tv-xz])\1", r"\1", text))  # ck -> k; rr stays
    out[stressed] = _cap(out[stressed])
    return "-".join(out)


class Respeller:
    """Respells words and phrases; `overrides` maps lower-case words (or phrases) to respellings."""

    def __init__(self, overrides: dict[str, str] | None = None):
        self.overrides: dict[str, str] = {}
        for key, value in (overrides or {}).items():
            spelled = " ".join(str(value).split())
            if not RESPELLING_RE.match(spelled):
                raise ValueError(f"respelling of '{key}' must be ASCII letters joined by hyphens or "
                                 f"spaces: '{value}'")
            self.overrides[" ".join(_tokens(str(key))) or _norm(str(key))] = spelled
        self._span = max((k.count(" ") + 1 for k in self.overrides), default=1)

    @classmethod
    def from_file(cls, path: str) -> "Respeller":
        try:
            data = yamlio.load(path) or {}
        except FileNotFoundError:
            data = {}
        if not isinstance(data, dict):
            raise yamlio.YamlError(path, 1, "expected a mapping with an 'overrides:' key")
        overrides = data.get("overrides") or {}
        if not isinstance(overrides, dict):
            raise yamlio.YamlError(path, getattr(overrides, "line", 1),
                                   "'overrides' must map words to respellings")
        try:
            return cls({str(k): str(v) for k, v in overrides.items()})
        except ValueError as exc:
            raise yamlio.YamlError(path, getattr(overrides, "line", 1), str(exc)) from None

    def word(self, word: str) -> str:
        """Respelling of one word said on its own (a phrase is passed to `text`)."""
        tokens = _tokens(word)
        if len(tokens) != 1:
            return self.text(word)
        return self._word(tokens[0])

    def text(self, text: str) -> str:
        """Respelling of a phrase or sentence: words separated by one space."""
        tokens = _tokens(text)
        out: list[str] = []
        i = 0
        while i < len(tokens):
            for span in range(min(self._span, len(tokens) - i), 1, -1):
                key = " ".join(tokens[i:i + span])
                if key in self.overrides:
                    out.append(self.overrides[key])
                    i += span
                    break
            else:
                token = tokens[i]
                spelled = self._word(token)
                if len(tokens) > 1 and token in FUNCTION_WORDS and token not in self.overrides:
                    spelled = spelled.lower()
                if spelled:
                    out.append(spelled)
                i += 1
        if len(out) > 1 and not any(ch.isupper() for ch in "".join(out)):
            out[-1] = out[-1].upper()  # a phrase of function words only (lo que): stress the last
        return " ".join(out)

    def _word(self, token: str) -> str:
        for key in (token, token.replace("-", "").replace("'", "").replace("’", "")):
            if key in self.overrides:
                return self.overrides[key]
        letters = _letters(token)
        return _respell(letters) if letters else ""

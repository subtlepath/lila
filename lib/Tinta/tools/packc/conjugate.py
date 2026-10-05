"""Spanish conjugator: five persons (no vosotros), the tenses of forms.TENSES,
gerund and participle.

Regular verbs, stem-change consequences and spelling changes follow from the
infinitive; what cannot be inferred (which verbs change their stem, irregular
yo forms, strong preterites, ...) comes from content/verbs/irregular.yaml,
whose schema is documented at the top of that file.

    conj = Conjugator.from_file("content/verbs/irregular.yaml")
    c = conj.conjugate("llamarse")       # raises ConjugationError
    c.forms[("pres", "1s")] == "me llamo"
    c.forms[("imp", "2s")] == "llámate"; c.forms[("impneg", "2s")] == "te llames"
    c.gerund == "llamándose"; c.participle == "llamado"
    c.irregular, c.stem ("" | "e>ie" | "o>ue" | "e>i" | "u>ue" | "i>ie"), c.spelling, c.reflexive

Negative imperatives are stored without "no". Orthography follows the RAE
2010 rules (monosyllables such as `rio`, `vio`, `hui` carry no accent).
"""

from __future__ import annotations

from dataclasses import dataclass, field

from . import phon, yamlio
from .forms import IMPERATIVE_PERSONS, PERSONS, TENSES

REFLEXIVE_PRONOUNS = {"1s": "me", "2s": "te", "3s": "se", "1p": "nos", "3p": "se"}
STEM_CLASSES = ("", "e>ie", "o>ue", "e>i", "u>ue", "i>ie")
SPEC_KEYS = ("stem", "accent", "yo", "pret", "fut", "subj", "imp", "gerund", "participle", "forms", "like",
             "extra", "regular")

_ENDINGS = {
    "ar": {"pres": ("o", "as", "a", "amos", "an"), "pret": ("é", "aste", "ó", "amos", "aron"),
           "impf": ("aba", "abas", "aba", "ábamos", "aban"), "subj": ("e", "es", "e", "emos", "en")},
    "er": {"pres": ("o", "es", "e", "emos", "en"), "pret": ("í", "iste", "ió", "imos", "ieron"),
           "impf": ("ía", "ías", "ía", "íamos", "ían"), "subj": ("a", "as", "a", "amos", "an")},
    "ir": {"pres": ("o", "es", "e", "imos", "en"), "pret": ("í", "iste", "ió", "imos", "ieron"),
           "impf": ("ía", "ías", "ía", "íamos", "ían"), "subj": ("a", "as", "a", "amos", "an")},
}
_FUT = ("é", "ás", "á", "emos", "án")
_COND = ("ía", "ías", "ía", "íamos", "ían")
_STRONG_PRET = ("e", "iste", "o", "imos", "ieron")
_STRESSED = ("1s", "2s", "3s", "3p")  # persons whose present and subjunctive stress the stem


class ConjugationError(ValueError):
    pass


@dataclass
class Conjugation:
    infinitive: str
    gerund: str
    participle: str
    forms: dict[tuple[str, str], str] = field(default_factory=dict)
    irregular: bool = False
    stem: str = ""
    spelling: bool = False
    reflexive: bool = False
    # Forms outside the table that still belong to the verb, with the cell they
    # stand for: haber's impersonal hay -> ("pres", "3s"). Linked, never drilled.
    extra: dict[str, tuple[str, str]] = field(default_factory=dict)


def _last_vowel(stem: str) -> int:
    """Index of the stem's last vowel, skipping the silent u of a final gu/qu (seguir)."""
    end = len(stem)
    if stem.endswith(("gu", "qu")):
        end -= 1
    for i in range(end - 1, -1, -1):
        if stem[i] in "aeiou":
            return i
    return -1


def _parse_tag(key: str, verb: str) -> tuple[str, str]:
    tense, _, person = str(key).partition(".")
    if person in ("2p", "2pl"):
        raise ConjugationError(f"{verb}: '{key}' is a vosotros form; Mexican Spanish has none")
    if tense not in TENSES or person not in PERSONS:
        raise ConjugationError(f"{verb}: unknown form '{key}' (use tense.person, e.g. pres.1s)")
    if tense in ("imp", "impneg") and person not in IMPERATIVE_PERSONS:
        raise ConjugationError(f"{verb}: no imperative for person {person}")
    return tense, person


class _Builder:
    """Conjugates one non-reflexive infinitive from rules plus its yaml spec."""

    def __init__(self, infinitive: str, spec: dict):
        self.inf = infinitive
        self.spec = spec
        ending = phon.strip_accents(infinitive[-2:])
        if len(infinitive) < 2 or ending not in _ENDINGS or not infinitive.isalpha():
            raise ConjugationError(f"'{infinitive}' is not an infinitive")
        self.conj = ending
        self.stem = infinitive[:-2]
        self.cls = str(spec.get("stem", "") or "")
        if self.cls not in STEM_CLASSES:
            raise ConjugationError(f"{infinitive}: unknown stem change '{self.cls}' ({', '.join(STEM_CLASSES[1:])})")
        self.accent = bool(spec.get("accent", False))
        self.spelling = self.accent
        self.uir = self.conj == "ir" and self.stem.endswith("u") and not self.stem.endswith(("gu", "qu"))

    # -- stems -----------------------------------------------------------------

    def _change(self, stem: str, strong: bool) -> str:
        """Apply the stem change: strong in stem-stressed forms, weak in the -ir contexts."""
        if not self.cls:
            return stem
        i = _last_vowel(stem)
        old = self.cls[0]
        if i < 0 or stem[i] != old:
            raise ConjugationError(f"{self.inf}: stem change {self.cls} needs an '{old}' in the stem")
        if strong:
            new = self.cls.split(">")[1]
            if new == "ue" and i == 0:
                new = "hue"  # oler -> huelo
        elif self.conj == "ir" and self.cls in ("e>ie", "e>i", "o>ue"):
            new = "u" if old == "o" else "i"
        else:
            return stem
        return stem[:i] + new + stem[i + 1:]

    def _accented(self, stem: str) -> str:
        """enviar -> enví-, continuar -> continú-, reunir -> reún-, reír -> rí-."""
        for i in range(len(stem) - 1, -1, -1):
            if stem[i] in "iu":
                return stem[:i] + phon.ACUTE[stem[i]] + stem[i + 1:]
        raise ConjugationError(f"{self.inf}: 'accent' needs an i or u in the stem")

    def strong(self) -> str:
        stem = self._change(self.stem, True)
        return self._accented(stem) if self.accent else stem

    def weak(self) -> str:
        return self._change(self.stem, False)

    # -- joining stems and endings -------------------------------------------------

    def join(self, stem: str, ending: str) -> str:
        """Stem + ending with the spelling rules that depend on both."""
        first = ending[:1]
        out = None
        if self.conj == "ar" and first in ("e", "é"):
            for old, new in (("gu", "gü"), ("c", "qu"), ("g", "gu"), ("z", "c")):
                if stem.endswith(old):
                    out = stem[:-len(old)] + new + ending
                    break
        elif self.conj != "ar" and first in ("a", "o"):
            if stem.endswith("c"):
                vowel_before = len(stem) > 1 and phon.strip_accents(stem[-2]) in "aeiou"
                out = stem[:-1] + ("zc" if vowel_before else "z") + ending
            elif stem.endswith("gu"):
                out = stem[:-2] + "g" + ending
            elif stem.endswith("qu"):
                out = stem[:-2] + "c" + ending
            elif stem.endswith("g"):
                out = stem[:-1] + "j" + ending
        if out is not None:
            self.spelling = True
            return out
        if self.conj != "ar":
            # The u of gu/qu is silent (seguir -> siguió), so it is not a vowel here.
            tail = "" if stem.endswith(("gu", "qu")) else stem[-1:]
            if self.uir and first in ("a", "e", "o"):
                self.spelling = True
                return stem + "y" + ending  # construyo, construye, construya
            if ending.startswith("i") and len(ending) > 1 and ending[1] in "eoó":
                if tail in ("a", "e", "o", "u"):
                    self.spelling = True
                    return stem + "y" + ending[1:]  # leyó, leyeron, leyendo, construyó
                if tail == "i" or stem.endswith(("ñ", "ll")):
                    return stem + ending[1:]  # rieron, riendo, gruñó
            if tail in ("a", "e", "o") and ending.startswith("i") and not ending.startswith(("ió", "ie")):
                return stem + "í" + ending[1:]  # leíste, oímos, leído
        return stem + ending

    @staticmethod
    def mono(form: str) -> str:
        """Monosyllables take no written accent (vio, rio, hui), RAE 2010."""
        if phon.has_accent(form) and len(phon.syllabify(form)) == 1:
            return phon.strip_accents(form)
        return form

    # -- tenses -----------------------------------------------------------------

    def build(self) -> Conjugation:
        spec, end = self.spec, _ENDINGS[self.conj]
        forms: dict[tuple[str, str], str] = {}
        irregular = any(k not in ("stem", "accent", "regular") for k in spec)

        # Present.
        for person, ending in zip(PERSONS, end["pres"]):
            stem = self.strong() if person in _STRESSED else self.stem
            forms[("pres", person)] = self.join(stem, ending)

        # Preterite.
        pret = spec.get("pret")
        if pret is None and self.inf.endswith("ducir"):
            pret, irregular = self.stem[:-1] + "j", True  # conducir -> conduje
        if pret:
            pret = str(pret)
            for person, ending in zip(PERSONS, _STRONG_PRET):
                if person == "3p" and pret.endswith("j"):
                    ending = "eron"  # dijeron, trajeron
                stem = pret[:-1] + "z" if ending == "o" and pret.endswith("c") else pret  # hizo
                forms[("pret", person)] = stem + ending
        else:
            for person, ending in zip(PERSONS, end["pret"]):
                stem = self.weak() if person in ("3s", "3p") else self.stem
                forms[("pret", person)] = self.join(stem, ending)

        for person, ending in zip(PERSONS, end["impf"]):
            forms[("impf", person)] = self.stem + ending
        fut = str(spec.get("fut") or phon.strip_accents(self.inf))
        for person, ending in zip(PERSONS, _FUT):
            forms[("fut", person)] = fut + ending
        for person, ending in zip(PERSONS, _COND):
            forms[("cond", person)] = fut + ending

        # Present subjunctive: an explicit stem, else the irregular yo form's stem,
        # else the regular stems (stem change in stressed persons, -ir change in 1p).
        subj_stem = spec.get("subj")
        if subj_stem is None and spec.get("yo"):
            yo = str(spec["yo"])
            if yo.endswith("o"):
                subj_stem = yo[:-1]
        for person, ending in zip(PERSONS, end["subj"]):
            if subj_stem is not None:
                forms[("subj", person)] = str(subj_stem) + ending
            else:
                stem = self.strong() if person in _STRESSED else self.weak()
                forms[("subj", person)] = self.join(stem, ending)

        forms = {key: self.mono(form) for key, form in forms.items()}
        if spec.get("yo"):
            forms[("pres", "1s")] = str(spec["yo"])

        overrides = spec.get("forms") or {}
        for key, value in overrides.items():
            forms[_parse_tag(key, self.inf)] = str(value)

        # Imperatives come from the (possibly overridden) present and subjunctive.
        forms[("imp", "2s")] = forms[("pres", "3s")]
        for person in ("3s", "3p"):
            forms[("imp", person)] = forms[("subj", person)]
        for person in IMPERATIVE_PERSONS:
            forms[("impneg", person)] = forms[("subj", person)]
        for person, value in (spec.get("imp") or {}).items():
            forms[_parse_tag(f"imp.{person}", self.inf)] = str(value)
        for key, value in overrides.items():
            if str(key).startswith("imp"):
                forms[_parse_tag(key, self.inf)] = str(value)

        if self.conj == "ar":
            gerund = self.stem + "ando"
        else:
            gerund = self.join(self.weak(), "iendo")
        participle = self.join(self.stem, "ado" if self.conj == "ar" else "ido")
        extra = {str(form): _parse_tag(tag, self.inf) for form, tag in (spec.get("extra") or {}).items()}
        return Conjugation(
            infinitive=self.inf, gerund=str(spec.get("gerund") or gerund),
            participle=str(spec.get("participle") or participle), forms=forms, irregular=irregular,
            stem=self.cls, spelling=self.spelling, extra=extra)


def _prefixed(prefix: str, form: str) -> str:
    """Prefix a form, keeping its stress: tén -> mantén, puse -> compuse, rio -> sonrió."""
    if not form:
        return form
    syllables = phon.syllabify(form)
    from_end = len(syllables) - 1 - phon.stress_index(form, syllables)
    word = phon.syllabify(prefix + form)
    return phon.place_accent(word, len(word) - 1 - from_end)


class Conjugator:
    def __init__(self, irregular: dict | None = None):
        self.irregular = {str(k): (v or {}) for k, v in (irregular or {}).items()}
        for verb, spec in self.irregular.items():
            if not isinstance(spec, dict):
                raise ConjugationError(f"{verb}: entry must be a mapping")
            for key in spec:
                if key not in SPEC_KEYS:
                    raise ConjugationError(f"{verb}: unknown key '{key}' (allowed: {', '.join(SPEC_KEYS)})")

    @classmethod
    def from_file(cls, path: str) -> "Conjugator":
        try:
            data = yamlio.load(path) or {}
        except FileNotFoundError:
            data = {}
        return cls(dict(data))

    def conjugate(self, infinitive: str) -> Conjugation:
        infinitive = infinitive.strip()
        reflexive = infinitive.endswith("se") and phon.strip_accents(infinitive[-4:-2]) in ("ar", "er", "ir")
        base = infinitive[:-2] if reflexive else infinitive
        conj = self._plain(base, set())
        if not reflexive:
            return conj
        forms = {}
        for (tense, person), form in conj.forms.items():
            pronoun = REFLEXIVE_PRONOUNS[person]
            forms[(tense, person)] = phon.attach(form, pronoun) if tense == "imp" else f"{pronoun} {form}"
        return Conjugation(infinitive=infinitive, gerund=phon.attach(conj.gerund, "se"),
                           participle=conj.participle, forms=forms, irregular=conj.irregular,
                           stem=conj.stem, spelling=conj.spelling, reflexive=True, extra=conj.extra)

    def _plain(self, infinitive: str, seen: set) -> Conjugation:
        spec = self.irregular.get(infinitive, {})
        like = spec.get("like")
        if not like:
            return _Builder(infinitive, spec).build()
        like = str(like)
        if like in seen or not infinitive.endswith(like) or infinitive == like:
            raise ConjugationError(f"{infinitive}: 'like: {like}' must name a shorter verb it ends with")
        base = self._plain(like, seen | {infinitive})
        prefix = infinitive[:-len(like)]
        forms = {key: _prefixed(prefix, form) for key, form in base.forms.items()}
        if spec.get("fut"):
            for person, f_end, c_end in zip(PERSONS, _FUT, _COND):
                forms[("fut", person)] = str(spec["fut"]) + f_end
                forms[("cond", person)] = str(spec["fut"]) + c_end
        own = {k: v for k, v in spec.items() if k in ("forms", "imp")}
        for key, value in (own.get("forms") or {}).items():
            forms[_parse_tag(key, infinitive)] = str(value)
        for person, value in (own.get("imp") or {}).items():
            forms[_parse_tag(f"imp.{person}", infinitive)] = str(value)
        return Conjugation(
            infinitive=infinitive, gerund=str(spec.get("gerund") or _prefixed(prefix, base.gerund)),
            participle=str(spec.get("participle") or _prefixed(prefix, base.participle)), forms=forms,
            irregular=base.irregular, stem=base.stem, spelling=base.spelling)

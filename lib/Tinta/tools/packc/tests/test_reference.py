"""Conjugator against an external reference (Fred Jehle's verb database).

Skipped unless build/reference/jehle_verb_database.csv exists; fetch it with
`python3 tools/packc/tests/fetch_reference.py` (licence notes there).

The reference is compared for every verb whose conjugation pattern is known to
the conjugator: verbs in content/verbs/irregular.yaml, and verbs with no
irregularity that our rules cannot infer (all -ar/-er/-ir verbs that the
reference shows with an unchanged stem in the present). Vosotros columns are
ignored. Known differences, where the reference is not what Tinta teaches,
are listed in ACCEPTED.
"""

from __future__ import annotations

import csv
import os
import unittest

from packc import phon
from packc.build import known_verb_patterns, verb_pattern
from packc.conjugate import ConjugationError, Conjugator
from packc.forms import PERSONS

from .fetch_reference import PATH
from .test_conjugate import course_conjugator

MOODS = {("Indicativo", "Presente"): "pres", ("Indicativo", "Pretérito"): "pret",
         ("Indicativo", "Imperfecto"): "impf", ("Indicativo", "Futuro"): "fut",
         ("Indicativo", "Condicional"): "cond", ("Subjuntivo", "Presente"): "subj",
         ("Imperativo Afirmativo", "Presente"): "imp", ("Imperativo Negativo", "Presente"): "impneg"}
COLUMNS = {"1s": "form_1s", "2s": "form_2s", "3s": "form_3s", "1p": "form_1p", "3p": "form_3p"}

# (verb, tag) -> reason the reference form is not used. Tags as in forms.py, or ger/part.
ACCEPTED = {
    ("huir", "pret.1s"): "the reference predates RAE 2010, which writes the monosyllable hui",
    ("criar", "pret.1s"): "the reference predates RAE 2010, which writes the monosyllable crie",
    ("criarse", "pret.1s"): "the reference predates RAE 2010, which writes the monosyllable crie",
    ("freír", "pret.3s"): "the reference predates RAE 2010, which writes the monosyllable frio",
    ("maquillarse", "imp.2s"): "reference error: 'te maquíllate'",
    ("maquillarse", "imp.3s"): "reference error: 'te maquíllese'",
    ("secarse", "imp.2s"): "reference error: rows copied from equivocarse",
    ("secarse", "imp.3s"): "reference error: rows copied from equivocarse",
    ("secarse", "imp.3p"): "reference error: rows copied from equivocarse",
}


def load_reference() -> dict[str, dict[str, str]]:
    verbs: dict[str, dict[str, str]] = {}
    with open(PATH, encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            tense = MOODS.get((row["mood"], row["tense"]))
            if tense is None:
                continue
            forms = verbs.setdefault(row["infinitive"], {})
            for person, column in COLUMNS.items():
                value = row[column].strip()
                if tense in ("imp", "impneg") and person not in ("2s", "3s", "3p"):
                    continue
                if tense == "impneg":
                    # A few rows hold a bare "no" where the verb has no such form.
                    value = value[3:] if value.startswith("no ") else ""
                forms[f"{tense}.{person}"] = value
            forms["ger"] = row["gerund"].strip()
            forms["part"] = row["pastparticiple"].strip()
    return verbs


def is_plain(forms: dict[str, str], infinitive: str) -> bool:
    """True when the reference's present stem is the infinitive's (no undeclared stem change)."""
    stem = phon.strip_accents(infinitive[:-2])
    yo = phon.strip_accents(forms.get("pres.1s", ""))
    three = phon.strip_accents(forms.get("pres.3s", ""))
    return yo[:len(stem)] == stem[:len(yo)] and three.startswith(stem[:-1] if stem.endswith("c") else stem)


@unittest.skipUnless(os.path.exists(PATH), "reference not downloaded (tools/packc/tests/fetch_reference.py)")
class Reference(unittest.TestCase):
    def test_against_reference(self):
        conj = course_conjugator()  # every declared verb, fragments included
        reference = load_reference()
        declared = set(conj.irregular)
        compared = agreed = 0
        verbs = 0
        problems: list[str] = []
        for verb, forms in sorted(reference.items()):
            base = verb[:-2] if verb.endswith("se") and verb[-4:-2] in ("ar", "er", "ir", "ír") else verb
            if base not in declared and not is_plain(forms, base):
                continue
            if verb != base:
                continue  # the database lists reflexives with their pronouns; base verbs suffice
            try:
                c = conj.conjugate(verb)
            except ConjugationError as exc:
                problems.append(f"{verb}: {exc}")
                continue
            verbs += 1
            for tag, want in forms.items():
                if not want or (verb, tag) in ACCEPTED:
                    continue
                if tag == "ger":
                    have = c.gerund
                elif tag == "part":
                    have = c.participle
                else:
                    tense, person = tag.split(".")
                    have = c.forms.get((tense, person), "")
                compared += 1
                if have in want.split(", "):  # "imprimido, impreso": either is right
                    agreed += 1
                else:
                    problems.append(f"{verb} {tag}: reference {want}, ours {have}")
        print(f"\nreference: {verbs} verbs, {compared} forms compared, {agreed} agree "
              f"({100 * agreed / max(compared, 1):.2f}%)")
        self.assertGreater(verbs, 300)
        self.assertEqual(problems, [], "\n".join(problems[:60]))

    def test_verb_pattern_list(self):
        """tools/packc/data/verb_patterns.tsv lists exactly the reference verbs the rules get wrong."""
        patterns = known_verb_patterns()
        missing, false_alarms, wrong_class = [], [], []
        for verb, forms in sorted(load_reference().items()):
            reflexive = verb.endswith("se") and verb[-4:-2] in ("ar", "er", "ir", "ír")
            base = verb[:-2] if reflexive else verb
            regular = agrees({}, base, verb, forms)
            listed = verb_pattern(base, patterns)
            if not regular and listed is None:
                missing.append(base)
            elif regular and listed is not None:
                false_alarms.append(f"{base} ({listed[0]})")
            elif listed is not None and listed[0] != "irregular":
                spec = {"accent": True} if listed[0] == "accent" else {"stem": listed[0].split()[0]}
                if listed[0].endswith("accent"):
                    spec["accent"] = True
                if not agrees(spec, base, verb, forms):
                    wrong_class.append(f"{base} ({listed[0]})")
        self.assertEqual(missing, [], "not regular but not listed")
        self.assertEqual(false_alarms, [], "listed but regular")
        self.assertEqual(wrong_class, [], "listed pattern does not fix it")


def agrees(spec: dict, base: str, verb: str, forms: dict[str, str]) -> bool:
    """Whether `verb`, with `spec` as its irregular.yaml entry, matches the reference."""
    try:
        c = Conjugator({base: spec} if spec else {}).conjugate(verb)
    except ConjugationError:
        return False
    for tag, want in forms.items():
        if not want or (verb, tag) in ACCEPTED:
            continue
        if tag == "ger":
            have = c.gerund
        elif tag == "part":
            have = c.participle
        else:
            have = c.forms.get(tuple(tag.split(".")), "")
        if have not in want.split(", "):
            return False
    return True


if __name__ == "__main__":
    unittest.main()

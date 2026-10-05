"""Conjugator against the hand-checked golden table and targeted cases."""

from __future__ import annotations

import os
import unittest

from packc.conjugate import ConjugationError, Conjugator
from packc.forms import PERSONS

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
IRREGULAR = os.path.join(ROOT, "content", "verbs", "irregular.yaml")
GOLDEN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "golden", "verbs.tsv")
FRAGMENTS = os.path.join(ROOT, "content", "verbs", "irregular", "*.yaml")


def course_conjugator() -> Conjugator:
    """The conjugator the compiler uses: irregular.yaml merged with its per-unit fragments."""
    import glob  # noqa: PLC0415
    from packc import yamlio  # noqa: PLC0415

    merged: dict = {}
    for path in [IRREGULAR] + sorted(glob.glob(FRAGMENTS)):
        merged.update(yamlio.load(path) or {})
    return Conjugator(merged)


def load_golden() -> dict[str, dict[str, list[str]]]:
    table: dict[str, dict[str, list[str]]] = {}
    with open(GOLDEN, encoding="utf-8") as fh:
        for line in fh:
            if not line.strip() or line.startswith("#"):
                continue
            verb, tense, *cells = line.rstrip("\n").split("\t")
            table.setdefault(verb, {})[tense] = cells
    return table


class GoldenTable(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.conj = Conjugator.from_file(IRREGULAR)
        cls.golden = load_golden()

    def test_table_is_large_enough(self):
        self.assertGreaterEqual(len(self.golden), 40)
        for verb, rows in self.golden.items():
            self.assertEqual(sorted(rows), sorted(["pres", "pret", "impf", "fut", "cond", "subj", "imp",
                                                   "impneg", "ger", "part"]), verb)

    def test_every_form(self):
        mismatches = []
        for verb, rows in self.golden.items():
            c = self.conj.conjugate(verb)
            for tense, cells in rows.items():
                if tense == "ger":
                    got = [c.gerund]
                elif tense == "part":
                    got = [c.participle]
                else:
                    got = [c.forms.get((tense, p), "-") for p in PERSONS]
                for want, have in zip(cells, got):
                    if want != have:
                        mismatches.append(f"{verb} {tense}: expected {want}, got {have}")
        self.assertEqual(mismatches, [])


class Rules(unittest.TestCase):
    def setUp(self):
        self.conj = Conjugator.from_file(IRREGULAR)

    def test_no_vosotros_and_imperative_persons(self):
        c = self.conj.conjugate("hablar")
        self.assertEqual({p for (_t, p) in c.forms}, set(PERSONS))
        self.assertNotIn(("imp", "1s"), c.forms)
        self.assertNotIn(("imp", "1p"), c.forms)
        self.assertFalse(any(f.endswith(("áis", "éis")) for f in c.forms.values()))

    def test_flags(self):
        self.assertTrue(self.conj.conjugate("tener").irregular)
        self.assertFalse(self.conj.conjugate("hablar").irregular)
        self.assertEqual(self.conj.conjugate("pensar").stem, "e>ie")
        self.assertFalse(self.conj.conjugate("pensar").irregular)
        self.assertTrue(self.conj.conjugate("buscar").spelling)
        self.assertFalse(self.conj.conjugate("hablar").spelling)
        self.assertTrue(self.conj.conjugate("conducir").irregular)
        c = self.conj.conjugate("despertarse")
        self.assertTrue(c.reflexive)
        self.assertEqual(c.stem, "e>ie")
        self.assertEqual(c.forms[("pres", "1s")], "me despierto")
        self.assertEqual(c.forms[("imp", "2s")], "despiértate")

    def test_derived_verbs_keep_stress(self):
        c = self.conj.conjugate("obtener")
        self.assertEqual(c.forms[("imp", "2s")], "obtén")
        self.assertEqual(c.forms[("pret", "3s")], "obtuvo")
        self.assertEqual(self.conj.conjugate("predecir").forms[("imp", "2s")], "predice")
        self.assertEqual(self.conj.conjugate("deshacer").forms[("imp", "2s")], "deshaz")
        self.assertEqual(self.conj.conjugate("suponer").participle, "supuesto")

    def test_spelling_inferred(self):
        cases = {
            ("traducir", "pres", "1s"): "traduzco", ("traducir", "pret", "3p"): "tradujeron",
            ("distinguir", "pres", "1s"): "distingo", ("dirigir", "subj", "1p"): "dirijamos",
            ("creer", "pret", "3s"): "creyó", ("destruir", "subj", "3s"): "destruya",
            ("llegar", "pret", "1s"): "llegué", ("almorzar", "pres", "1s"): "almuerzo",
            ("almorzar", "pret", "1s"): "almorcé", ("checar", "subj", "3p"): "chequen",
            ("esquiar", "pres", "3s"): "esquía", ("graduar", "subj", "1s"): "gradúe",
        }
        for (verb, tense, person), want in cases.items():
            self.assertEqual(self.conj.conjugate(verb).forms[(tense, person)], want, (verb, tense, person))

    def test_errors(self):
        with self.assertRaises(ConjugationError):
            self.conj.conjugate("casa")
        with self.assertRaises(ConjugationError):
            Conjugator({"pensar": {"stem": "a>b"}}).conjugate("pensar")
        with self.assertRaises(ConjugationError):
            Conjugator({"pensar": {"forms": {"pres.2p": "pensáis"}}}).conjugate("pensar")
        with self.assertRaises(ConjugationError):
            Conjugator({"hablar": {"colour": "red"}})
        with self.assertRaises(ConjugationError):
            Conjugator({"hablar": {"stem": "o>ue"}}).conjugate("hablar")

    def test_explicit_forms_win(self):
        conj = Conjugator({"hablar": {"forms": {"pres.1s": "hablo!", "imp.2s": "habla!"}}})
        c = conj.conjugate("hablar")
        self.assertEqual(c.forms[("pres", "1s")], "hablo!")
        self.assertEqual(c.forms[("imp", "2s")], "habla!")


if __name__ == "__main__":
    unittest.main()


class GuardListCoversDeclarations(unittest.TestCase):
    """Every verb whose irregular.yaml entry changes its forms is on the guard list, so a
    later lexicon file cannot use it undeclared (tools/packc/data/verb_patterns.tsv)."""

    # Declared by choice, but the rules' form is also correct (imprimido and impreso).
    OPTIONAL = {"imprimir"}

    def test_declared_verbs_are_guarded(self):
        from packc.build import known_verb_patterns, verb_pattern  # noqa: PLC0415

        conj, plain, patterns = course_conjugator(), Conjugator(), known_verb_patterns()
        missing = []
        for verb, spec in sorted(conj.irregular.items()):
            if spec.get("regular") or verb in self.OPTIONAL or verb_pattern(verb, patterns):
                continue
            declared, ruled = conj.conjugate(verb), plain.conjugate(verb)
            if (declared.forms, declared.gerund, declared.participle) != (ruled.forms, ruled.gerund, ruled.participle):
                missing.append(verb)
        self.assertEqual(missing, [], "add these to tools/packc/data/verb_patterns.tsv")
